// secretstore.hpp
// SecretStore class definition for managing sensitive information.
// Allows for cross-platform storage of kvp secrets.

#include <string>
#include <libsecret/secret.h>

namespace SecretStore {
    static const SecretSchema schema = {
        "org.freedesktop.Secret.Generic",
        SECRET_SCHEMA_NONE,
        {
            { "key", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, SecretSchemaAttributeType{} }
        }
    };

    static std::string Get(const std::string &key) {
        GError *error = nullptr;
        std::string res;

        char *password = secret_password_lookup_sync(
            &schema,
            nullptr, // cancellable
            &error,
            "key",
            key.data(),
            nullptr
        );

        if (password) {
            res = std::string(password);
            secret_password_free(password);
        }

        return res;
    }
}