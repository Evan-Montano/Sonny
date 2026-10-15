// Implementation of the SimpleCurlWrapper class.

#include "simplecurlwrapper.hpp"
#include "../common/logger.hpp"
#include "common/jsonutil.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
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
        class CurlGlobal {
        public:
            CurlGlobal() {
                const CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
                if (result != CURLE_OK) {
                    throw std::runtime_error("Curl global initialization failed.");
                }
            }

            ~CurlGlobal() {
                curl_global_cleanup();
            }

            static size_t write_callback(char *ptr, const size_t size, const size_t nmemb, void *userdata) {
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
                const nlohmann::json ready = nlohmann::json::parse(
                    ReadLine(_responseFd, _responseBuffer)
                );
                const std::string type = ready.value("type", "");
                if (type != "ready") {
                    throw std::runtime_error(
                        ready.value("error", "Browser worker did not become ready")
                    );
                }
            }
            catch (...) {
                Stop(true);
                throw;
            }
        }

        ~BrowserProcess() {
            Stop(false);
        }

        BrowserProcess(const BrowserProcess&) = delete;
        BrowserProcess& operator=(const BrowserProcess&) = delete;

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
            nlohmann::json headerJson = nlohmann::json::object();
            for (const auto &[key, value] : headers) {
                headerJson[key] = value;
            }

            const std::uint64_t requestId = ++_nextRequestId;
            const nlohmann::json request = {
                {"type", "request"},
                {"id", requestId},
                {"url", url},
                {"headers", std::move(headerJson)}
            };

            // Writing bytes to node process.
            WriteAllWithoutSigpipe(_requestFd, request.dump() + '\n');

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
                    WriteAllWithoutSigpipe(
                        _requestFd,
                        nlohmann::json({{"type", "shutdown"}}).dump() + '\n'
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
        this->browserProcess.reset();
        if (this->curlHandle) {
            curl_easy_cleanup(this->curlHandle);
        }
    }

    SimpleCurlWrapper::SimpleCurlWrapper(const bool executeInBrowser)
        : ExecuteInBrowser(executeInBrowser), curlHandle(nullptr) {
        static CurlGlobal curlGlobal;
        (void)curlGlobal;

        this->curlHandle = curl_easy_init();
        if (this->curlHandle == nullptr) {
            throw std::runtime_error("Curl handle initialization failed.");
        }

        if (this->ExecuteInBrowser) {
            try {
                this->browserProcess = std::make_unique<BrowserProcess>();
            }
            catch (...) {
                curl_easy_cleanup(this->curlHandle);
                this->curlHandle = nullptr;
                throw;
            }
        }
    }

    CurlRequestEnum SimpleCurlWrapper::ExecuteHttpRequest(CurlRequest &request) {
        std::lock_guard<std::mutex> lock(this->executionMutex);

        if (this->ExecuteInBrowser) {
            std::string body;
            try {
                body = this->browserProcess->Execute(
                    request.GetFormattedUri(),
                    request.GetHeaders()
                );
            }
            catch (const std::exception &error) {
                Logger::Error(
                    std::format("Browser request failed: {}", error.what()),
                    true
                );
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
            if (request.ClearSettingsAfterUse) {
                request.Reset();
            }
            return CurlRequestEnum::SUCCESS;
        }

        // 2. Get reference to the formatted uri.
        const std::string& uriWithParams = request.GetFormattedUri();

        // 3. Reset curl handle.
        curl_easy_reset(this->curlHandle);

        // 4. Configure the curl handle.
        curl_easy_setopt(this->curlHandle, CURLOPT_URL, uriWithParams.c_str()); // set url
        curl_easy_setopt(this->curlHandle, CURLOPT_CA_CACHE_TIMEOUT, 604800L);

        // Set headers
        curl_slist *list = nullptr;
        for (const auto& [key, value] : request.GetHeaders()) {
            const std::string headerStr = std::format("{}: {}", key, value);
            list = curl_slist_append(list, headerStr.c_str());
        }
        curl_easy_setopt(this->curlHandle, CURLOPT_HTTPHEADER, list);

        // set callback function
        if (request.CallbackSet) {
            curl_easy_setopt(this->curlHandle, CURLOPT_WRITEFUNCTION, CurlGlobal::write_callback);
            curl_easy_setopt(this->curlHandle, CURLOPT_WRITEDATA, &request);
        }

        // 5. Call easy perform.
        const CURLcode result = curl_easy_perform(this->curlHandle);

        if (result == CURLE_OK) {
            request.FinishResponse();
        }

        // 6. Cleanup request object.
        if (request.ClearSettingsAfterUse) {
            request.Reset();
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

}
