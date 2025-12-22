import {$, commandWithProgress, download, log, programFlash, programSram, sleep, SRAM_SIZE} from './index';

const BLOCK_SIZE = 32 * 1024;
const PASS = ' ✅';
const FAIL = ' ❌';
let expectedFlashBuffer;
let expectedSramBuffer;

function fillWith(size, byte = 0xff) {
    const buffer = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
        buffer[i] = byte;
    }
    return buffer;
}

function fillRandom(size) {
    const buffer = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
        buffer[i] = Math.floor(Math.random() * 0x100);
    }
    return buffer;
}

function assertEqual(in1, in2) {
    const buffer1 = new Uint8Array(in1);
    const buffer2 = new Uint8Array(in2);
    if (buffer1.length !== buffer2.length) {
        throw new Error('Sizes differ');
    }
    for (let i = 0; i < buffer1.length; i++) {
        if (buffer1[i] !== buffer2[i]) {
            throw new Error(`Mismatch at offset 0x${i.toString(16)}`);
        }
    }
}


async function assertFlashFirstBlockBlank() {
    assertEqual(
        await download(BLOCK_SIZE, `D${BLOCK_SIZE}\r`),
        fillWith(BLOCK_SIZE, 0xff)
    );
}

async function assertSramBlank() {
    assertEqual(
        await download(SRAM_SIZE, `Ds\r`),
        fillWith(SRAM_SIZE, 0xff)
    );
}


async function assertFlashEraseAndWrite(size = BLOCK_SIZE) {
    await commandWithProgress(`E${size}\r`, 'Erase');
    const expected = fillRandom(size);
    await programFlash(expected);
    const actual = await download(size, `D${size}\r`);
    assertEqual(expected, actual);

    // Clean up after
    await commandWithProgress(`E${size}\r`, 'Erase');
}

async function assertSramEraseAndWrite() {
    const size = SRAM_SIZE;
    const expected = fillRandom(size);
    await programSram(expected);
    const actual = await download(size, `Ds\r`);
    assertEqual(expected, actual);
}

$('input.qa-flash').addEventListener('change', async ({target: {files}}) => {
    if (!files || files.length === 0) return;
    expectedFlashBuffer = new Uint8Array(await files[0].arrayBuffer());
});

$('input.qa-sram').addEventListener('change', async ({target: {files}}) => {
    if (!files || files.length === 0) return;
    expectedSramBuffer = new Uint8Array(await files[0].arrayBuffer());
});

async function runTest(name, promise, target) {
    log(`${name}\r`, true);
    try {
        await promise;
        log('\rPASS\r', true);
        if (target) target.innerText += PASS;
    } catch (err) {
        log(`\rFAIL: ${err}\r`, true);
        if (target) target.innerText += FAIL;
    }
}

/**
 * Test 0 - on building - Quickly verify the flash is blank, all 0xff
 * Soldering errors can result in non-ff. Passing this test doesn't mean everything's ok.
 */
$('button.qa-blank').addEventListener('click', async ({target}) => {
    await runTest('Asserting first block of flash blank', assertFlashFirstBlockBlank(), target);
});

/**
 * Test 1 - Quickly fill a block with random stuff and see if it can be read back
 * Quickly determines if flash cannot read and write.
 */
$('button.qa-quick-flash').addEventListener('click', async ({target}) => {
    await runTest('Erase/write/verify flash with one block of random', assertFlashEraseAndWrite(BLOCK_SIZE), target);
});

/**
 * Test 2 - Fill SRAM with random stuff and see if it can be read back.
 * Ensures SRAM can read and write.
 */
$('button.qa-sram').addEventListener('click', async ({target}) => {
    await runTest('Write/verify SRAM with random', assertSramEraseAndWrite(BLOCK_SIZE), target);
});

$('button.qa-program-both').addEventListener('click', async ({target}) => {
    if (!expectedFlashBuffer) {
        console.error('Set expected file first');
        return;
    }
    await programFlash(expectedFlashBuffer);
    if (!expectedSramBuffer) {
        console.error('Set expected file first');
        return;
    }
    await programSram(expectedSramBuffer);
});

$('button.qa-program-flash-multi').addEventListener('click', async ({target}) => {
    if (!expectedFlashBuffer) {
        console.error('Set expected file first');
        return;
    }
    const MAX = 20;
    await runTest('Flashing repeatedly', (async () => {
        for (let i = 0; i < MAX; i++) {
            // await commandWithProgress(`E\r`, 'Erasing Flash');
            // await sleep(1000);
            log(`\nFlash ${i+1}/${MAX}\n`);
            await programFlash(expectedFlashBuffer);
            await sleep(5000);
        }
        log('\nVerifying data...\n');
        return assertEqual(
            await download(expectedFlashBuffer.byteLength, `D${expectedFlashBuffer.byteLength}\r`),
            expectedFlashBuffer
        );
    })(), target);
});


/**
 * Test 3 - Go program it with a game, test on console. Come back. Now verify its contents are what you expect.
 * Running on console is a strong signal that it's okay. This is stronger.
 */
$('button.qa-verify-flash').addEventListener('click', async ({target}) => {
    // const bank = 0x200000;
    // const bytesToDownload = 1024 * 640;
    // const actualBuffer = await download(bytesToDownload, `D${bank - bytesToDownload / 2}/${bytesToDownload}\r`);
    // saveBufferToFile(actualBuffer, 'test.bin');

    if (!expectedFlashBuffer) {
        console.error('Set expected file first');
        return;
    }
    await runTest('Verifying flash contents', (async () => assertEqual(
        await download(expectedFlashBuffer.byteLength, `D${expectedFlashBuffer.byteLength}\r`),
        expectedFlashBuffer
    ))(), target);
});

/**
 * Test 4 - Ensure SRAM maintained the expected contents surviving unpowered.
 */
$('button.qa-verify-sram').addEventListener('click', async ({target}) => {
    if (!expectedSramBuffer) {
        console.error('Set expected file first');
        return;
    }
    await runTest('Verifying SRAM contents', (async () => assertEqual(
        await download(SRAM_SIZE, `Ds\r`),
        expectedSramBuffer
    ))(), target);
});

/**
 * Tests complete - format everything. Verify it's clear as expected.
 * We already tested erasing, but this final check also prepares the floopy drive for shipping.
 */
$('button.qa-wipe').addEventListener('click', async ({target}) => {
    await commandWithProgress(`E\r`, 'Erasing Flash');
    await sleep(100);
    await commandWithProgress(`Es\r`, 'Erasing SRAM');
    await sleep(100);
    await commandWithProgress(`Sf\r`, 'Formatting Filesystem');
    await assertFlashFirstBlockBlank();
    await assertSramBlank();
    log('WIPED!', true);
    target.innerText += PASS;
});
