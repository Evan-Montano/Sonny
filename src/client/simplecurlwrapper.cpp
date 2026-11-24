// Implementation of the SimpleCurlWrapper class.

#include "simplecurlwrapper.hpp"
#include "../common/logger.hpp"
#include "common/jsonutil.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>

#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Client {

    namespace {
        /**
         * @brief Initializes curl for the process and supplies the response write callback.
         */
        class CurlGlobal {
        public:
            // CONSTRUCTOR

            CurlGlobal() {
                const CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
                if (result != CURLE_OK) {
                    throw std::runtime_error("Curl global initialization failed.");
                }
            }

            // DESTRUCTOR

            ~CurlGlobal() {
                curl_global_cleanup();
            }

            // METHODS

            static size_t WriteCallback(char *ptr, const size_t size, const size_t nmemb, void *userdata) {
                CurlRequest *request = static_cast<CurlRequest*>(userdata);
                const size_t bytes = size * nmemb;

                if (request) {
                    request->AppendResponse(std::string_view(ptr, bytes));
                }
                return bytes;
            }
        };

        void CloseFd(int &fd) noexcept {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }

        /**
         * @brief Writes the full request to the pipe connected to the node process.
         * 
         * @param fd 
         * @param data 
         */
        void WriteAllWithoutSigpipe(const int fd, const std::string &data) {
            sigset_t blockedSignals;
            sigset_t previousMask;
            sigset_t pendingSignals;
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGPIPE);

            const int maskResult = pthread_sigmask(
                SIG_BLOCK,
                &blockedSignals,
                &previousMask
            );
            if (maskResult != 0) {
                throw std::system_error(
                    maskResult,
                    std::generic_category(),
                    "Unable to block SIGPIPE while writing to browser worker"
                );
            }

            sigpending(&pendingSignals);
            const bool sigpipeWasPending = sigismember(&pendingSignals, SIGPIPE) == 1;

            // To prevent partial writes, we are loopoing until all bytes are sent.
            std::size_t written = 0;
            int writeError = 0;
            while (written < data.size()) {
                const ssize_t result = write(
                    fd,
                    data.data() + written,
                    data.size() - written
                );

                if (result > 0) {
                    written += static_cast<std::size_t>(result);
                }
                else if (result < 0 && errno == EINTR) {
                    continue;
                }
                else {
                    writeError = result == 0 ? EPIPE : errno;
                    break;
                }
            }

            if (writeError == EPIPE && sigpipeWasPending == false) {
                timespec timeout{0, 0};
                sigtimedwait(&blockedSignals, nullptr, &timeout);
            }

            const int restoreResult = pthread_sigmask(
                SIG_SETMASK,
                &previousMask,
                nullptr
            );
            if (writeError != 0) {
                throw std::system_error(
                    writeError,
                    std::generic_category(),
                    "Unable to write to browser worker"
                );
            }
            if (restoreResult != 0) {
                throw std::system_error(
                    restoreResult,
                    std::generic_category(),
                    "Unable to restore signal mask after browser worker write"
                );
            }
        }

        std::string ReadLine(const int fd, std::string &buffer) {
            while (true) {
                const std::size_t newline = buffer.find('\n');
                if (newline != std::string::npos) {
                    std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    return line;
                }

                char chunk[4096];
                const ssize_t result = read(fd, chunk, sizeof(chunk));
                if (result == 0) {
                    throw std::runtime_error(
                        "Browser worker closed its response pipe"
                    );
                }
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "Unable to read from browser worker"
                    );
                }

                buffer.append(chunk, static_cast<std::size_t>(result));
            }
        }
    }

    /**
     * @brief Owns a browser worker process and exchanges framed JSON requests with it.
     */
    class SimpleCurlWrapper::BrowserProcess {
    public:
        // CONSTRUCTORS
        BrowserProcess() {
            // Create two file descriptors, one for RW on the parent process
            // and another for the browser process.
            int toChild[2]{-1, -1};
            int fromChild[2]{-1, -1};

            if (pipe2(toChild, O_CLOEXEC) != 0) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "Unable to create browser worker request pipe"
                );
            }

            if (pipe2(fromChild, O_CLOEXEC) != 0) {
                const int error = errno;
                close(toChild[0]);
                close(toChild[1]);
                throw std::system_error(
                    error,
                    std::generic_category(),
                    "Unable to create browser worker response pipe"
                );
            }

            // Cloning the current process and saving to this private member.
            _processId = fork();

            if (_processId < 0) {
                const int error = errno;
                close(toChild[0]);
                close(toChild[1]);
                close(fromChild[0]);
                close(fromChild[1]);
                throw std::system_error(
                    error,
                    std::generic_category(),
                    "Unable to fork browser worker"
                );
            }

            // ===== CHILD PROCESS ===== //
            if (_processId == 0) {
                // We don't need these fd.
                close(toChild[1]);
                close(fromChild[0]);

                // Setting the read op in the toChild fd to 
                // the standard input of this process.
                // And the write op in the fromChild fd to the standard output.
                if (dup2(toChild[0], STDIN_FILENO) < 0 ||
                    dup2(fromChild[1], STDOUT_FILENO) < 0) {
                    _exit(126);
                }

                close(toChild[0]);
                close(fromChild[1]);

#ifdef SONNY_BROWSER_CLIENT_PATH
                char *const arguments[] = {
                    const_cast<char*>("node"),
                    const_cast<char*>(SONNY_BROWSER_CLIENT_PATH),
                    nullptr
                };
#else
                char *const arguments[] = {
                    const_cast<char*>("node"),
                    const_cast<char*>("src/client/browserclient.js"),
                    nullptr
                };
#endif
                execvp(arguments[0], arguments); // Child process is  switched here.
                _exit(127);
            } // ===== END CHILD PROCESS ===== //

            // ===== PARENT PROCESS ===== //
            close(toChild[0]);
            close(fromChild[1]);
            _requestFd = toChild[1];
            _responseFd = fromChild[0];

            try {
                // Waiting for the browser instance to spin up. Node will be 
                // sending a "ready" notification.
                const Common::JsonUtility ready(
                    ReadLine(_responseFd, _responseBuffer)
                );
                std::string type;
                ready.TryGet("type", type);
                if (type != "ready") {
                    std::string error = "Browser worker did not become ready";
                    ready.TryGet("error", error);
                    throw std::runtime_error(error);
                }
            }
            catch (...) {
                Stop(true);
                throw;
            }
        }

        BrowserProcess(const BrowserProcess&) = delete;
        BrowserProcess& operator=(const BrowserProcess&) = delete;

        // DESTRUCTOR

        ~BrowserProcess() {
            Stop(false);
        }

        // METHODS
        /**
         * @brief Takes in curl request parameters and sends them to the node-browser instance.
         * After the response, the node process returns the body to this method, where we return the response.
         * 
         * @param url 
         * @param headers 
         * @return std::string Curl request body.
         */
        std::string Execute(const std::string &url, const HeaderMap &headers) {
            const std::uint64_t requestId = ++_nextRequestId;
            Common::JsonUtility request;
            request.Set("type", "request");
            request.Set("id", requestId);
            request.Set("url", url);
            request.Set("headers", headers);

            // Writing bytes to node process.
            WriteAllWithoutSigpipe(_requestFd, request.ToString() + '\n');

            // Capturing the response and deserializing into a json object.
            const Common::JsonUtility response(ReadLine(_responseFd, _responseBuffer));

            if (response.Get<std::uint64_t>("id") != requestId) {
                throw std::runtime_error(
                    "Browser worker returned a mismatched response ID"
                );
            }

            const std::string type = response.Get<std::string>("type");
            if (type != "response") {
                throw std::runtime_error(
                    response.Get<std::string>("error")
                );
            }

            return response.Get<std::string>("body");
        }

    private:
        // MEMBERS
        int _requestFd = -1;
        int _responseFd = -1;
        pid_t _processId = -1;
        std::uint64_t _nextRequestId = 0;
        std::string _responseBuffer;

        // METHODS
        void Stop(const bool terminateImmediately) noexcept {
            if (_processId <= 0) {
                CloseFd(_requestFd);
                CloseFd(_responseFd);
                return;
            }

            if (terminateImmediately == false && _requestFd >= 0) {
                try {
                    Common::JsonUtility shutdown;
                    shutdown.Set("type", "shutdown");
                    WriteAllWithoutSigpipe(
                        _requestFd,
                        shutdown.ToString() + '\n'
                    );
                    (void)ReadLine(_responseFd, _responseBuffer);
                }
                catch (...) {
                    // The worker may already have exited; reap it below.
                }
            }

            CloseFd(_requestFd);
            CloseFd(_responseFd);

            int status = 0;
            if (terminateImmediately) {
                kill(_processId, SIGTERM);
            }

            pid_t result = waitpid(_processId, &status, WNOHANG);
            for (int attempt = 0; result == 0 && attempt < 200; ++attempt) {
                usleep(10000);
                result = waitpid(_processId, &status, WNOHANG);
            }

            if (result == 0) {
                kill(_processId, SIGTERM);
                do {
                    result = waitpid(_processId, &status, 0);
                } while (result < 0 && errno == EINTR);
            }
            else if (result < 0 && errno == EINTR) {
                do {
                    result = waitpid(_processId, &status, 0);
                } while (result < 0 && errno == EINTR);
            }

            _processId = -1;
        }
    };

    SimpleCurlWrapper::~SimpleCurlWrapper() {
        this->BrowserWorker.reset();
        if (this->CurlHandle) {
            curl_easy_cleanup(this->CurlHandle);
        }
    }

    SimpleCurlWrapper::SimpleCurlWrapper(const bool executeInBrowser)
        : ExecuteInBrowser(executeInBrowser), CurlHandle(nullptr) {
        static CurlGlobal curlGlobal;
        (void)curlGlobal;

        this->CurlHandle = curl_easy_init();
        if (this->CurlHandle == nullptr) {
            throw std::runtime_error("Curl handle initialization failed.");
        }

        if (this->ExecuteInBrowser) {
            try {
                this->BrowserWorker = std::make_unique<BrowserProcess>();
            }
            catch (...) {
                curl_easy_cleanup(this->CurlHandle);
                this->CurlHandle = nullptr;
                throw;
            }
        }
    }

    CurlRequestEnum SimpleCurlWrapper::ExecuteHttpRequest(CurlRequest &request) {
        std::lock_guard<std::mutex> lock(this->ExecutionMutex);

        if (this->ExecuteInBrowser) {
            std::string body;
            try {
                body = this->BrowserWorker->Execute(
                    request.GetFormattedUri(),
                    request.GetHeaders()
                );
            }
            catch (const std::exception &error) {
                Logger::Error(
                    std::format("Browser request failed: {}", error.what()),
                    true
                );
                request.RecordResult(0, error.what());
                if (request.ClearSettingsAfterUse) {
                    request.Reset();
                }
                return CurlRequestEnum::UNKNOWN;
            }

            // Append the response to the callback.
            if (request.CallbackSet) {
                request.AppendResponse(body);
            }
            request.FinishResponse();
            request.RecordResult(0, "");
            if (request.ClearSettingsAfterUse) {
                request.Reset();
            }
            return CurlRequestEnum::SUCCESS;
        }

        // 2. Get reference to the formatted uri.
        const std::string& uriWithParams = request.GetFormattedUri();

        // 3. Reset curl handle.
        curl_easy_reset(this->CurlHandle);

        // 4. Configure the curl handle.
        curl_easy_setopt(this->CurlHandle, CURLOPT_URL, uriWithParams.c_str()); // set url
        curl_easy_setopt(this->CurlHandle, CURLOPT_CA_CACHE_TIMEOUT, 604800L);
        curl_easy_setopt(this->CurlHandle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(this->CurlHandle, CURLOPT_SSL_VERIFYHOST, 2L);

        // Set body. The request owns the bytes, so they stay valid through curl_easy_perform.
        if (request.HasBody()) {
            const std::string& body = request.GetBody();
            curl_easy_setopt(this->CurlHandle, CURLOPT_POST, 1L);
            curl_easy_setopt(this->CurlHandle, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(this->CurlHandle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }

        // Set timeouts. Signals cannot be used to interrupt a timed out request on a worker thread.
        if (request.GetTimeout().count() > 0 || request.GetConnectTimeout().count() > 0) {
            curl_easy_setopt(this->CurlHandle, CURLOPT_TIMEOUT, static_cast<long>(request.GetTimeout().count()));
            curl_easy_setopt(this->CurlHandle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.GetConnectTimeout().count()));
            curl_easy_setopt(this->CurlHandle, CURLOPT_NOSIGNAL, 1L);
        }

        // Set headers
        curl_slist *list = nullptr;
        for (const auto& [key, value] : request.GetHeaders()) {
            std::string headerStr = std::format("{}: {}", key, value);
            list = curl_slist_append(list, headerStr.c_str());
            if (request.SensitiveData) {
                CurlRequest::WipeString(headerStr);
            }
        }
        curl_easy_setopt(this->CurlHandle, CURLOPT_HTTPHEADER, list);

        // set callback function
        if (request.CallbackSet) {
            curl_easy_setopt(this->CurlHandle, CURLOPT_WRITEFUNCTION, CurlGlobal::WriteCallback);
            curl_easy_setopt(this->CurlHandle, CURLOPT_WRITEDATA, &request);
        }

        // 5. Call easy perform.
        const CURLcode result = curl_easy_perform(this->CurlHandle);

        if (result == CURLE_OK) {
            request.FinishResponse();
        }

        long responseCode = 0;
        curl_easy_getinfo(this->CurlHandle, CURLINFO_RESPONSE_CODE, &responseCode);
        request.RecordResult(responseCode, result == CURLE_OK ? "" : curl_easy_strerror(result));

        // 6. Cleanup request object.
        if (request.ClearSettingsAfterUse) {
            request.Reset();
        }
        if (request.SensitiveData) {
            for (curl_slist *header = list; header != nullptr; header = header->next) {
                if (header->data != nullptr) {
                    explicit_bzero(header->data, std::strlen(header->data));
                }
            }
        }
        curl_slist_free_all(list);

        // 7. Handle errors
        CurlRequestEnum res;
        switch (result) {
            case CURLcode::CURLE_OK:
                res = CurlRequestEnum::SUCCESS;
                break;
            default:
                res = CurlRequestEnum::UNKNOWN;
                break;
        }

        return res;
    }

    std::string SimpleCurlWrapper::EncodeUrl(std::string_view value) {
        std::string res{};

        CURL* curl = curl_easy_init();
        if (curl) {
            char* encoded = curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
            res = encoded != nullptr ? encoded : "";
            
            if (encoded) curl_free(encoded);
            curl_easy_cleanup(curl);
        }

        return res;
    }

}
