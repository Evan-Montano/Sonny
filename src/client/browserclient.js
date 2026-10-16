// browserclient.js
// This file contains the code for the subprocess used by the Simple Curl Wrapper 
// object to call http requests in a live browser due to stinky AWS challenges.
'use strict';

const readline = require('node:readline');
const { chromium } = require('playwright');

function send(message) {
    process.stdout.write(`${JSON.stringify(message)}\n`);
}

function isChallengePage(body) {
    const normalized = body.toLowerCase();
    return normalized.includes("verify that you're not a robot") ||
        normalized.includes('javascript is disabled');
}

async function executeRequest(page, request) {
    const response = await page.goto(request.url, {
        waitUntil: 'domcontentloaded',
        timeout: 30000,
        headers: request.headers
    });

    if (!response) {
        throw new Error('No initial HTTP response received');
    }

    // Dukascopy's successful browser test receives an initial 202 while the
    // AWS challenge runs. Give that challenge the same time to complete.
    let body = await page.locator('body').innerText();
    if (response.status() === 202 || isChallengePage(body)) {
        await page.waitForTimeout(5000);
        body = await page.locator('body').innerText();
    }

    if (isChallengePage(body)) {
        throw new Error('AWS WAF challenge remains unresolved');
    }

    return body;
}

async function main() {
    const browser = await chromium.launch({ headless: true });

    try {
        const context = await browser.newContext();
        const page = await context.newPage();
        const input = readline.createInterface({
            input: process.stdin,
            crlfDelay: Infinity
        });

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

            if (request && typeof request === 'object' &&
                request.type === 'shutdown') {
                send({ type: 'stopped' });
                break;
            }

            if (request === null || typeof request !== 'object' ||
                request.type !== 'request' ||
                !Number.isSafeInteger(request.id) ||
                typeof request.url !== 'string' ||
                typeof request.headers !== 'object' ||
                request.headers === null ||
                Array.isArray(request.headers)) {
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
    process.stderr.write(`Browser worker failed: ${error.stack || error.message}\n`);
    send({ type: 'startup_error', error: error.message });
    process.exitCode = 1;
});
