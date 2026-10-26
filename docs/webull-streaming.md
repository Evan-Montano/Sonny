# Webull OpenAPI quote streaming

Sonny can start one SPY ETF `QUOTE` stream in either production or sandbox. It uses a unique MQTT client/session ID, TLS-verified MQTT 3.1.1 on port 1883, and signed HTTPS subscribe/unsubscribe calls. Connection, HTTP subscription, and message receipt are separate states in the log. Received quote payloads are currently logged only by topic, local receive time, and byte count; Sonny does not decode the protobuf quote payload or feed it into predictions.

## Requirements and application setup

On Fedora install the development and keyring tools:

```bash
sudo dnf install libsecret-devel libsecret-tools openssl-devel pkgconf-pkg-config
```

Sonny builds Paho MQTT C++ v1.6.0 and its bundled Paho MQTT C dependency v1.3.16+ through CMake FetchContent, as static libraries with TLS enabled. Normal Sonny build and tests do not need credentials, a running keyring, or Webull network access. CMake configuration needs access to the already configured FetchContent dependencies.

Apply for a Webull OpenAPI application through the [OpenAPI getting-started guide](https://developer.webull.com/apis/docs/getting-started/) and generate its App Key and App Secret in the Webull OpenAPI management portal. Sandbox and live credentials are kept in separate Secret Service entries. SPY quote access requires a Webull OpenAPI market-data entitlement; a Webull platform quote subscription does not grant that entitlement. Without it, the signed request is still sent and the returned HTTP status and safe error diagnostic are logged. See Webull's [OpenAPI advanced quotes subscription guide](https://developer.webull.com/apis/docs/market-data-api/subscribe-quotes/) for market-data access.

Webull's [authentication](https://developer.webull.com/apis/docs/authentication/overview/), [signature](https://developer.webull.com/apis/docs/authentication/signature/), and [token](https://developer.webull.com/apis/docs/authentication/token/) documentation says each HTTPS request is signed with HMAC-SHA1 using the App Secret. A reusable access token is additionally required only when application 2FA is enabled. If 2FA is enabled, create and verify a token through Webull's documented Token flow, then provision it under `access_token`; Sonny does not create or refresh tokens. Webull documents tokens as becoming invalid after 15 consecutive days without API calls, so treat this as a refreshable credential, not a permanent application secret. Without 2FA, `app_key` and `app_secret` are sufficient and `access_token` is optional.

## Provision secrets

Run the commands in a desktop session with an unlocked Secret Service provider such as GNOME Keyring. `secret-tool store` prompts for the value; do not put secrets on the command line.

```bash
secret-tool store --label='Sonny Webull sandbox app key' application sonny provider webull environment sandbox name app_key
secret-tool store --label='Sonny Webull sandbox app secret' application sonny provider webull environment sandbox name app_secret
secret-tool store --label='Sonny Webull sandbox access token' application sonny provider webull environment sandbox name access_token

secret-tool store --label='Sonny Webull live app key' application sonny provider webull environment live name app_key
secret-tool store --label='Sonny Webull live app secret' application sonny provider webull environment live name app_secret
secret-tool store --label='Sonny Webull live access token' application sonny provider webull environment live name access_token
```

The required entries are `app_key` and `app_secret` for the selected environment. Provision `access_token` only when 2FA is enabled. The stable attributes are `application=sonny`, `provider=webull`, `environment=live|sandbox`, and `name=app_key|app_secret|access_token`. SecretStore is read-only. Missing entries are reported by credential name and the selected environment; Sonny does not fall back to the other environment, plaintext files, or environment variables. A locked or unavailable keyring is reported with setup guidance.

## Run and understand the stream

Build and launch normally. Choose **Start Live Webull SPY Quote Stream** from the main menu or **Start Sandbox Webull SPY Quote Stream** from Setup. Choose **Stop Webull Quote Stream** to stop. Exiting the application also stops and joins the streaming worker.

The log distinguishes MQTT connect success from HTTP subscription success. HTTP 401 is classified as authentication failure; 403 is reported as permission failure unless its safe error text indicates an authentication problem; 400/422 is an invalid request. This is classification based on the returned status and sanitized error fields, so Webull may provide more detail in its response. Missing credentials, unavailable Secret Service, MQTT connection failures, reconnects, and unsubscribe results are logged separately. A subscription accepted response means only that Webull accepted the request. Only a `Webull message received` entry proves a payload arrived.

Sandbox streaming uses `api.sandbox.webull.com` for subscription management and `data-api.sandbox.webull.com:1883` for MQTT. The production pair is `api.webull.com` and `data-api.webull.com:1883`. Sandbox may provide delayed data, and Webull documents that paid OpenAPI data entitlements are separate from ordinary Webull subscriptions. Testing data delivery therefore depends on Webull's current sandbox availability, credentials, market-data access, and market conditions.

## Verified protocol and documentation caveat

Webull's current Data Streaming guide documents MQTT 3.1.1, App Key as username, arbitrary password, unique MQTT ClientId/session ID, TLS at port 1883, then HTTP subscription. The signature uses UTC ISO timestamp, unique nonce, required signing headers, uppercase MD5 of the exact compact JSON body, URL-encoded canonical request, and Base64 HMAC-SHA1 keyed with `App Secret + "&"`. HTTPS headers are `x-app-key`, `x-timestamp`, `x-signature`, `x-signature-algorithm`, `x-signature-version`, `x-signature-nonce`, and `x-version: v3`; `x-access-token` is sent if provisioned.

There are overlapping Webull reference pages: the current Data Streaming guide gives the common MQTT/session flow, while one generated Subscribe reference shows a legacy `category_symbols` schema and another shows `symbols`, `category`, `sub_types`, and `grab`. Sonny uses the latter shape requested for this SPY quote, with `category=US_ETF`, `sub_types=[QUOTE]`, `grab="true"`, and `overnight_required=false`. The reference page's rendered enum currently lists `US_STOCK` but the broader Webull market-data docs list ETFs and `US_ETF`; the ETF category should be confirmed against a real authenticated API response once credentials are available. The successful compile and unit tests do not verify Webull's live endpoint behavior.
