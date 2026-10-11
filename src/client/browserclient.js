// browserclient.js
// This file contains the code for the subprocess used by the Simple Curl Wrapper 
// object to call http requests in a live browser due to stinky AWS challenges.

const { chromium } = require('playwright');

/**
 * A little test method to make sure playwright is working for the dukascopy url
 */
async function test() {
    const browser = await chromium.launch({
        headless: false
    });

    try {
        const page = await browser.newPage();

        const url =
            'https://jetta.dukascopy.com/v1/instruments/SPY.US-USD';

        // Log each request to the instrument endpoint, including reloads.
        page.on('response', async response => {
            if (response.url().includes('/v1/instruments/SPY.US-USD')) {
                console.error(
                    `Instrument request: HTTP ${response.status()}`
                );
            }
        });

        console.error(`Navigating to ${url}`);

        const initialResponse = await page.goto(url, {
            waitUntil: 'domcontentloaded',
            timeout: 30000
        });

        if (!initialResponse) {
            throw new Error('No initial HTTP response received');
        }

        console.error(
            `Initial HTTP status: ${initialResponse.status()}`
        );

        // Allow the WAF challenge and any subsequent reload to run.
        console.error('Waiting for challenge processing...');
        await page.waitForTimeout(5000);

        console.error('Final URL:', page.url());
        console.error('Final title:', await page.title());

        // Read the final rendered page, not the initial response object.
        const body = await page.locator('body').innerText();

        console.error(`Final body size: ${body.length} characters`);
        console.error('Final body preview:');
        console.error(body.slice(0, 1000));

        // Capture the most recent HTTP response for the endpoint.
        // The response listener above logs its status; the rendered body
        // alone does not establish the final HTTP status.
        if (!body.trim()) {
            throw new Error('Final page body is empty');
        }

        if (
            body.toLowerCase().includes("verify that you're not a robot") ||
            body.toLowerCase().includes('javascript is disabled')
        ) {
            throw new Error(
                'AWS WAF challenge remains unresolved'
            );
        }

        // Output the rendered page text for inspection.
        process.stdout.write(body + '\n');
    } finally {
        await browser.close();
    }
}

test().catch(error => {
    console.error('Error:', error.message);
    process.exitCode = 1;
});