// secretstore.cpp
// Reads credentials without introducing an application-managed copy on disk.

#include "secretstore.hpp"

#include <libsecret/secret.h>
#include <glib.h>
#include <gio/gio.h>
#include <string>

namespace Common {
    namespace {
        // Stable attributes let secret-tool provision entries without storing them in Sonny settings.
        constexpr SecretSchema WebullSchema = {
            "org.sonny.credentials",
            SECRET_SCHEMA_NONE,
            {
                {"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {"provider", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {"environment", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {"name", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}
            }
        };

        std::string EnvironmentName(SecretEnvironment environment) {
            return environment == SecretEnvironment::Live ? "live" : "sandbox";
        }
    }

    SecretResult SecretStore::GetWebullSecret(SecretEnvironment environment, std::string_view name) {
        SecretResult result;
        result.credential_name = std::string(name);
        GError* error = nullptr;
        gchar* secret = secret_password_lookup_sync(
            &WebullSchema,
            nullptr,
            &error,
            "application", "sonny",
            "provider", "webull",
            "environment", EnvironmentName(environment).c_str(),
            "name", result.credential_name.c_str(),
            nullptr
        );

        if (error != nullptr) {
            const std::string message = error->message != nullptr ? error->message : "Secret Service lookup failed";
            const std::string lower = [&] {
                std::string copy = message;
                for (char& c : copy) c = static_cast<char>(g_ascii_tolower(c));
                return copy;
            }();
            const bool locked = (error->domain == SECRET_ERROR && error->code == SECRET_ERROR_IS_LOCKED) ||
                lower.find("locked") != std::string::npos;
            const bool service_unavailable =
                (error->domain == G_DBUS_ERROR &&
                    (error->code == G_DBUS_ERROR_SERVICE_UNKNOWN || error->code == G_DBUS_ERROR_NAME_HAS_NO_OWNER ||
                     error->code == G_DBUS_ERROR_NO_REPLY)) ||
                (error->domain == G_IO_ERROR &&
                    (error->code == G_IO_ERROR_NOT_CONNECTED || error->code == G_IO_ERROR_CONNECTION_CLOSED)) ||
                lower.find("not available") != std::string::npos || lower.find("no such interface") != std::string::npos;
            result.error = locked ? SecretError::Locked :
                (service_unavailable ? SecretError::ServiceUnavailable : SecretError::LookupFailed);
            result.detail = result.error == SecretError::Locked
                ? "Secret Service is locked. Unlock the desktop keyring and retry."
                : (result.error == SecretError::ServiceUnavailable
                    ? "Secret Service is unavailable. Start a compatible desktop keyring and retry."
                    : "Secret Service lookup failed.");
            g_error_free(error);
            if (secret != nullptr) secret_password_free(secret);
            return result;
        }

        if (secret == nullptr || *secret == '\0') {
            result.error = SecretError::Missing;
            result.detail = "Secret is not provisioned.";
            if (secret != nullptr) secret_password_free(secret);
            return result;
        }

        result.value.assign(secret);
        result.has_value = true;
        secret_password_free(secret);
        return result;
    }
}
