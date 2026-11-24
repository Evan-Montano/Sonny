// secretstore.hpp
// Keeps API credentials in the desktop Secret Service instead of Sonny settings.

#pragma once

#include <string>
#include <string_view>

namespace Common {
    enum class SecretEnvironment { Live, Sandbox };

    enum class SecretError { Missing, ServiceUnavailable, Locked, LookupFailed };

    /**
     * @brief Returns either a credential value or the reason a Secret Service lookup failed.
     */
    struct SecretResult {
        // MEMBERS

        std::string Value;
        SecretError Error = SecretError::Missing;
        bool HasValue = false;
        std::string CredentialName;
        std::string Detail;

        // OPERATORS

        explicit operator bool() const noexcept { return HasValue; }
    };

    /**
     * @brief Retrieves credentials from the desktop Secret Service.
     */
    class SecretStore {
    public:
        // CONSTRUCTOR

        /**
         * @brief Prevents instantiation of this static lookup API.
         */
        SecretStore() = delete;

        // METHODS

        /**
         * @brief Retrieve a Webull credential from Secret Service.
         * Environment is part of the lookup so credentials cannot cross between live and sandbox.
         *
         * @param environment Live or sandbox credential scope.
         * @param name Credential name, such as app_key or app_secret.
         * @return The secret value or a lookup error.
         */
        static SecretResult GetWebullSecret(SecretEnvironment environment, std::string_view name);
    };
}
