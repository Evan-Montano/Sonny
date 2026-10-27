// browserclient.js
// This file contains the code for the subprocess used by the Simple Curl Wrapper
// object to call HTTP requests in a live browser due to AWS challenges.
'use strict';

const readline = require('node:readline');
const { chromium } = require('playwright');

const WIDGETS_URL = 'https://widgets.dukascopy.com/en/';

function send(message) {
    process.stdout.write(`${JSON.stringify(message)}\n`);
}

function log(message) {
    process.stderr.write(`[browserclient] ${message}\n`);
}

function isChallengePage(body) {
    const normalized = body.toLowerCase();

    return normalized.includes("verify that you're not a robot") ||
        normalized.includes('javascript is disabled');
}

async function executeRequest(page, request) {
    let result;

    for (let attempt = 1; attempt <= 2; ++attempt) {
        try {
            result = await page.evaluate(async (url) => {
                const response = await fetch(url, {
                    method: 'GET',
                    mode: 'cors',
                    credentials: 'omit',
                    cache: 'no-store',
                    headers: {
                        Accept: '*/*'
                    }
                });

                return {
                    status: response.status,
                    statusText: response.statusText,
                    url: response.url,
                    contentType: response.headers.get('content-type'),
                    body: await response.text()
                };
            }, request.url);

            break;
        } catch (error) {
            const navigationInterrupted =
                /Execution context was destroyed|most likely because of a navigation/i
                    .test(error.message);

            if (!navigationInterrupted || attempt === 2) {
                throw new Error(
                    `Request ${request.url} failed on attempt ${attempt}; ` +
                    `page URL=${page.url()}: ${error.message}`
                );
            }

            log(
                `Navigation interrupted request ${request.url}; ` +
                `reloading widget origin and retrying`
            );

            await page.goto(WIDGETS_URL, {
                waitUntil: 'domcontentloaded',
                timeout: 30000
            });
        }
    }

    if (!result) {
        throw new Error(`Request ${request.url} produced no result`);
    }

    log(
        `HTTP ${result.status} ${result.statusText}; ` +
        `content-type=${result.contentType}; url=${result.url}`
    );

    // Fetching a response does not execute challenge JavaScript.
    if (result.status === 202) {
        throw new Error(
            `Received HTTP 202 from ${result.url}. ` +
            `The response may require an AWS WAF challenge to run in the page. ` +
            `Body preview: ${result.body.slice(0, 300)}`
        );
    }

    if (result.status < 200 || result.status >= 300) {
        throw new Error(
            `HTTP ${result.status} ${result.statusText} from ${result.url}. ` +
            `Body preview: ${result.body.slice(0, 300)}`
        );
    }

    if (isChallengePage(result.body)) {
        throw new Error(
            'AWS WAF challenge response received instead of API data'
        );
    }

    return result.body;
}

async function main() {
    const browser = await chromium.launch({
        headless: true
    });

    try {
        const context = await browser.newContext();
        const page = await context.newPage();

        page.on('framenavigated', frame => {
            if (frame === page.mainFrame()) {
                log(`Main frame navigated to: ${frame.url()}`);
            }
        });

        page.on('requestfailed', request => {
            log(
                `Request failed: ${request.method()} ${request.url()} — ` +
                `${request.failure()?.errorText ?? 'unknown error'}`
            );
        });

        // Navigate to the final locale URL directly. The root URL redirects
        // to /en/, which can interrupt a concurrent navigation or page.evaluate.
        await page.goto(WIDGETS_URL, {
            waitUntil: 'domcontentloaded',
            timeout: 30000
        });

        log(`Browser ready at ${page.url()}`);

        const input = readline.createInterface({
            input: process.stdin,
            crlfDelay: Infinity
        });

        // Do not announce readiness until the page is initialized.
        send({ type: 'ready' });

        for await (const line of input) {
            let request;

            try {
                request = JSON.parse(line);
            } catch (error) {
                send({
                    type: 'error',
                    id: null,
                    error: `Invalid request JSON: ${error.message}`
                });
                continue;
            }

            if (
                request &&
                typeof request === 'object' &&
                request.type === 'shutdown'
            ) {
                send({ type: 'stopped' });
                break;
            }

            if (
                request === null ||
                typeof request !== 'object' ||
                request.type !== 'request' ||
                !Number.isSafeInteger(request.id) ||
                typeof request.url !== 'string' ||
                typeof request.headers !== 'object' ||
                request.headers === null ||
                Array.isArray(request.headers)
            ) {
                send({
                    type: 'error',
                    id: request && typeof request === 'object'
                        ? request.id ?? null
                        : null,
                    error: 'Malformed browser request'
                });
                continue;
            }

            try {
                const body = await executeRequest(page, request);

                send({
                    type: 'response',
                    id: request.id,
                    body
                });
            } catch (error) {
                send({
                    type: 'error',
                    id: request.id,
                    error: error.message
                });
            }
        }

        input.close();
        await context.close();
    } finally {
        await browser.close();
    }
}

main().catch(error => {
    // Keep stdout reserved for the machine-readable pipe protocol.
    process.stderr.write(
        `Browser worker failed: ${error.stack || error.message}\n`
    );

    send({
        type: 'startup_error',
        error: error.message
    });

    process.exitCode = 1;
});