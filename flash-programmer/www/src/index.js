import PACKAGE from '../package.json';
import {Serial} from './serial';
import {ADDR_HEADER_END, getCartDataFromHeader, lookupCartDatabase, parseRom, trimEnd} from './cart';
import ERROR_WEBUSB from 'bundle-text:./data/error-webusb.html';
import ERROR_CONNECT from 'bundle-text:./data/error-cant-connect.html';

console.log(`%c💞Floopy Drive interface ${PACKAGE.version}`, 'font-size: 200%');

// Warn if this doesn't match. Inserting this could be automated but that would require lockstep commits
// Something better could be done with build automation that builds Arduino and web
const FW_CURRENT = 'e569b1f';

const U32_MAX = 0xffffffff;
const SERIAL_BUFFER_SIZE = 64;
export const SRAM_SIZE = 0x20000;
export const FLASH_SIZE = 0x400000;
const UPLOAD_CHUNK_SIZE = 1024;
const TRUNCATE_DUMPED_SRAM = true;

const textDecoder = new TextDecoder();
/** @type Port */
let floopy;

export const $ = document.querySelector.bind(document);
const $body = $('body');
const $connectButton = $('button#connect');
const $lines = $('#receiver_lines');
const $statusDisplay = $('#progress-text');
const $progressInner = $('#progress-inner');
const $progressOuter = $('#progress-outer');

export function assert(b, message = "Unexpected state") {
    if (b !== true) {
        throw new Error(message);
    }
}

export function assertConnected() {
    assert(floopy?.isOpen === true, `Not connected. Please connect a Floopy Drive and use the Connect button.`);
}

export function sleep(ms) {
    return new Promise((resolve) => {
        setTimeout(resolve, ms);
    });
}

let currentOperationResolver;

function waitForStatus() {
    return new Promise(resolve => currentOperationResolver = resolve);
}

/**
 * Rounds to next integral serial buffer size
 * @return {number}
 */
function roundSize(s) {
    return Math.ceil(s / SERIAL_BUFFER_SIZE) * SERIAL_BUFFER_SIZE;
}

// The default port.onReceive handler. Parses text and puts it in the console.
function serialEcho(buffer) {
    if (ArrayBuffer.isView(buffer)) buffer = buffer.buffer;
    const data = (buffer instanceof Uint8Array) ? buffer : new Uint8Array(buffer);
    // trim null-terminated strings
    const lastByte = data.indexOf(0x00);
    const text = textDecoder.decode(lastByte < 0 ? data : data.subarray(0, lastByte));
    if (text.match(/^!OK\b/m)) {
        setProgress(false);
        if (typeof currentOperationResolver === 'function') {
            currentOperationResolver();
            currentOperationResolver = null;
        }
    }
    log(text);
}

function addLine(text) {
    const senderLine = document.createElement('div');
    senderLine.className = 'line';
    const textnode = document.createTextNode(text);
    senderLine.appendChild(textnode);
    $lines.appendChild(senderLine);
    return senderLine;
}

let currentReceiverLine;

function cls() {
    $lines.innerHTML = '';
    currentReceiverLine = null;
}

export function log(text, copyToConsole = false) {
    const lines = text.split('\r');
    if (currentReceiverLine) {
        currentReceiverLine.innerHTML = currentReceiverLine.innerHTML + lines.shift();
    }
    for (const line of lines) {
        currentReceiverLine = addLine(line);
    }
    if (copyToConsole) {
        console.log(text.replace('\r', '').trim());
    }
    $lines.scrollTo(0, $lines.scrollHeight, {smooth: true});
}

function setProgress(n) {
    if (typeof (n) === 'number') {
        $progressInner.classList.remove('done');
        $progressOuter.classList.remove('marquee');
        $progressInner.style.width = `${n * 100}%`;
    } else if (typeof (n) === 'boolean' && n === true) {
        $progressOuter.classList.add('marquee');
    } else {
        $progressOuter.classList.remove('marquee');
        $progressInner.style.width = '0';
    }
}

let isBusy = false;

function setBusy(b) {
    if (b) {
        $body.classList.add('busy');
    } else {
        $body.classList.remove('busy');
    }
    isBusy = b;
    $progressInner.style.width = '0';
    // Excludes dialog
    $('main').querySelectorAll('button').forEach(el => {
        if (!el.classList.contains('ignore-busy')) {
            el.disabled = b;
        }
    });
}

function setStatus(str) {
    $statusDisplay.innerHTML = str ?? '';
}

function showError(error) {
    const str = error?.message ?? error?.toString() ?? "Unknown error";
    const $dialog = $('#dialog-error');

    console.error(str);
    // log("ERROR: " + str + "\r\n");

    return new Promise((resolve) => {
        $('main').classList.add('dim');
        $dialog.querySelector('.body').innerHTML = str;
        $dialog.querySelectorAll('button').forEach(btn => {
            const result = btn.dataset['result'];
            btn.addEventListener('click', (event) => {
                $('main').classList.remove('dim');
                $dialog.classList.remove('active');
                resolve(result);
            }, {once: true});
        });
        $dialog.classList.add('active');
        $dialog.querySelector('button.default')?.focus();
    });
}

export async function commandWithProgress(command, stepInfo = '', indefinite = true) {
    assertConnected();
    setBusy(true);
    try {
        setProgress(indefinite || 0);
        setStatus(stepInfo);
        await floopy.send(command);
        await waitForStatus();
        setProgress();
        setStatus('Done');
    } finally {
        setBusy(false);
    }
}

export async function connect() {
    try {
        await floopy.connect();
        setStatus('Connected');
        $connectButton.classList.remove('default');

        // Parse initial connection message
        floopy.onReceive = (data) => {
            const text = textDecoder.decode(data);
            const match = text.match(/^!FW ([A-Za-z0-9]+)/);
            const fw = match?.[1];
            if (fw !== FW_CURRENT) {
                $('.download-firmware').classList.remove('hidden');
            }
            floopy.firmware = fw;
            floopy.onReceive = serialEcho;
            serialEcho(data);
        };
        floopy.onDisconnect = () => {
            setStatus('Disconnected.');
            $connectButton.classList.add('default');
        };
    } catch (error) {
        setStatus(error.message);
    }
}

export async function disconnect() {
    await floopy.disconnect();
    setStatus('Not connected');
    $connectButton.classList.add('default');
    window.floopy = floopy = null;
}

$connectButton?.addEventListener('click', async () => {
    if (floopy) {
        await disconnect();
    } else {
        try {
            window.floopy = floopy = await Serial.requestPort();
            await connect();
        } catch (error) {
            showError(ERROR_CONNECT).then();
            setStatus('Could not connect to Floopy Drive');
            console.error(error);
        }
    }
});

async function usbInit() {
    if (typeof navigator.usb !== 'object') {
        setStatus('ERROR: WebUSB not supported on this browser');
        await showError(ERROR_WEBUSB); // Should you be allowed to dismiss this?
        return;
    }
    const ports = await Serial.getPorts();
    if (ports.length === 0) {
        setStatus(`Floopy Drive not found. Connect drive and press the Connect button when notification appears.`);
    } else {
        setStatus('Connecting...');
        window.floopy = floopy = ports[0];
        await connect();
    }
}

// Connect on load
sleep(100).then(() => {
    usbInit().then(() => {
        navigator.usb?.addEventListener('connect', usbInit);
    });
});

// Warn on navigating away while busy
window.addEventListener('beforeunload', event => {
    if (isBusy) {
        event.preventDefault();
    }
});

// Attempt to disconnect cleanly when navigating away
window.addEventListener('unload', async () => {
    if (floopy?.isOpen) {
        // This is never going to actually complete, but it's a nice idea
        await disconnect();
    }
});

async function downloadAndParseCartHeader() {
    const size = roundSize(ADDR_HEADER_END);
    return getCartDataFromHeader(await download(size, `D${size}\r`));
}

export function download(bytesToDownload, serialCommand) {
    if (bytesToDownload % SERIAL_BUFFER_SIZE !== 0) {
        console.warn('Expected download size is not modulo serial buffer size');
    }
    return new Promise((resolve, reject) => {
        if (!floopy?.isOpen) {
            reject('Not connected');
        }
        const dumpBuffer = new ArrayBuffer(bytesToDownload);
        setStatus(`Dumping ${bytesToDownload} bytes...`);
        let addrBytes = 0;

        floopy.onReceive = (/** @type DataView */ data) => {
            const dumpView = new Uint8Array(dumpBuffer, addrBytes, data.byteLength);
            const packetView = new Uint8Array(data.buffer, 0, data.buffer.byteLength);
            console.assert(ArrayBuffer.isView(dumpView), 'Expected dumpView to be a view into the shared dumpBuffer');
            console.assert(ArrayBuffer.isView(packetView), 'Expected packetView to be a view into the shared data.buffer');

            dumpView.set(packetView);
            addrBytes += packetView.byteLength;

            setProgress(addrBytes / bytesToDownload);

            if (addrBytes >= dumpBuffer.byteLength) {
                floopy.onReceive = serialEcho;
                setProgress(0);
                setStatus('Done');
                resolve(dumpBuffer);
            }
        }

        floopy.send(serialCommand).then();
    });
}

export async function programFlash(buffer) {
    if (buffer.byteLength > FLASH_SIZE) {
        throw new Error(`ROM exceeds Flash size of ${FLASH_SIZE} bytes`);
    }
    // TODO pad to ensure modulo serial buffer size
    await floopy.send(`P${buffer.byteLength}\r`);
    await uploadChunked(buffer);
    if (floopy.firmware === FW_CURRENT) {
        await waitForStatus();
    } else {
        await sleep(250);
    }
}

export async function programSram(buffer) {
    if (buffer.byteLength > SRAM_SIZE) {
        throw new Error(`Save file exceeds SRAM size of ${SRAM_SIZE} bytes`);
    }
    // TODO pad to ensure modulo serial buffer size
    await floopy.send(`Ps${buffer.byteLength}\r`);
    await uploadChunked(buffer);
    if (floopy.firmware === FW_CURRENT) {
        await waitForStatus();
    } else {
        await sleep(250);
    }
}

async function uploadChunked(buffer) {
    try {
        for (let addr = 0; addr < buffer.byteLength;) {
            setProgress(addr / buffer.byteLength);
            const chunk = new Uint8Array(buffer.buffer, addr, Math.min(UPLOAD_CHUNK_SIZE, buffer.byteLength - addr));
            addr += chunk.byteLength;

            const {bytesWritten, status} = await floopy.send(chunk);
            if (status !== "ok") throw new Error(`${status}`);
            // console.log(`${status} writing ${block.byteLength} byte block starting at ${block.byteOffset}, ${bytesWritten} actually written`);
        }
        // console.log("DONE!");
    } finally {
        setProgress(0);
    }
}

export function saveBufferToFile(dumpBuffer, filename = 'loopy.bin') {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([dumpBuffer], {type: 'application/octet-stream'}));
    a.download = filename;
    a.click();
}

//------------------- BASIC UI -------------------

async function simpleFlash(/** @type File */ file) {
    try {
        assertConnected();
        setBusy(true);

        // check to see if dropped is a loopy bin
        // TODO eliminate word arrays
        let buffer = new Uint16Array(await file.arrayBuffer());
        let {header: newGameHeader, name: newGameName} = parseRom(buffer);
        newGameName ??= "Unrecognized ROM";
        const originalSize = buffer.byteLength;

        // Sanity checks
        if (originalSize > 0x400000) {
            throw new Error('Bigger than Floopy Drive flash!');
        }
        buffer = trimEnd(buffer);

        // What's on there right now?
        const currentRomHeader = await downloadAndParseCartHeader();
        const isDifferentGame = currentRomHeader.checksum !== newGameHeader.checksum;

        // If different game, backup SRAM
        if (isDifferentGame && currentRomHeader.checksum !== U32_MAX) {
            await commandWithProgress('Sr\r', 'Backing up current save');
        }

        // Erase either half or full - no speed benefit over erase exact number of bytes
        // if (originalSize <= 0x200000) {
        //     await commandWithProgress('E0\r', 'Erasing flash (half)');
        // } else {
        //     await commandWithProgress('E\r', 'Erasing flash (all)');
        // }

        // Erase only what's needed - most efficient
        await commandWithProgress(`E${buffer.byteLength}\r`, `Erasing flash`);

        // Flash
        setStatus(`Flashing <b>${newGameName}</b>...`);
        setBusy(true);
        await programFlash(buffer);

        // Restore old save or format SRAM if no existing backup
        if (isDifferentGame) {
            await commandWithProgress('Sw\r', 'Restoring previous save');
        }

        setStatus(`<span class="pink">Success! <span class="heart-icon"></span> You may disconnect your Floopy Drive or flash another game.</span>`);
        $('audio#ding')?.play();
        $drop.classList.add('success');
    } finally {
        setBusy(false);
    }
}

const $drop = $('.drop');
window.addEventListener('dragover', e => e.preventDefault());
window.addEventListener('drop', e => e.preventDefault());
$drop?.addEventListener('drop', event => {
    event.preventDefault();
    event.target.classList.remove('over');
    simpleFlash(event.dataTransfer.files?.[0]).then();
});
$drop?.addEventListener('click', () => $('.flash-upload-simple').click());
$drop?.addEventListener('dragover', (event) => event.target.classList.add('over'));
$drop?.addEventListener('dragleave', (event) => event.target.classList.remove('over'));
$drop?.addEventListener('dragend', (event) => event.target.classList.remove('over'));

const $advancedMode = $('#advanced-mode');
// restore initial state
if (localStorage.getItem('advanced') === 'true') {
    $advancedMode.setAttribute('open', 'open');
}
$advancedMode?.querySelector('summary')?.addEventListener('click', async event => {
    await sleep(0);
    const open = $advancedMode.hasAttribute('open');
    localStorage.setItem('advanced', open.toString());
});

//------------------- ADVANCED UI ----------------

$('.flash-upload-button')?.addEventListener('click', () => $('input.flash-upload').click());

$('input.flash-upload')?.addEventListener('change', async ({target, target: {files}}) => {
    try {
        if (!files || files.length === 0) {
            return;
        }
        assertConnected();
        setBusy(true);

        // TODO eliminate word arrays
        let buffer = new Uint16Array(await files[0].arrayBuffer());
        const info = parseRom(buffer);
        buffer = trimEnd(buffer);

        if (info) {
            setStatus(`Programming ${info.name} (${buffer.byteLength} bytes) to flash...`)
        } else {
            setStatus(`Programming ${buffer.byteLength} bytes to flash...`);
        }

        await programFlash(buffer);
        setStatus('Done');
    } finally {
        setBusy(false);
        // Allow set same file again
        target.value = null;
    }
});

$('input.flash-upload-simple')?.addEventListener('change', ({target: {files}}) => {
    if (!files || files.length === 0) {
        return;
    }
    simpleFlash(files[0]).then();
});

$('.sram-upload-button')?.addEventListener('click', () => $('input.sram-upload').click());
$('input.sram-upload')?.addEventListener('change', async ({target, target: {files}}) => {
    try {
        if (!files || files.length === 0) {
            return;
        }
        assertConnected();
        setBusy(true);

        const buffer = new Uint8Array(await files[0].arrayBuffer());
        assert(buffer.byteLength <= SRAM_SIZE, `Save expected to be ${SRAM_SIZE} bytes or less, was ${buffer.byteLength}`);

        setStatus(`Programming ${buffer.byteLength} bytes of SRAM...`);

        // TODO pad to ensure modulo serial buffer size
        await floopy.send(`Ps${buffer.byteLength}\r`);
        await uploadChunked(buffer);
        setProgress();
        setStatus('Done');
    } finally {
        // Allow set same file again
        setBusy(false);
        target.value = null;
    }
});

$('button.flash-inspect')?.addEventListener('click', async () => {
    try {
        assertConnected();
        setBusy(true);
        const header = await downloadAndParseCartHeader();
        // console.log(header);
        log(`Parsed header: ${JSON.stringify(header)}\r\n`);

        const cart = lookupCartDatabase(header.checksum);
        if (cart) {
            log(`Identified in database as: ${JSON.stringify(cart)}\r\n\r\n`);
            setStatus(`Current Loopy game: <b>${cart.name}</b>`);
        } else if (header.checksum === U32_MAX) {
            setStatus('Cart seems to be empty');
        } else {
            setStatus('Contents not matched in cart database');
        }

        await floopy.send('I\r');
    } finally {
        setBusy(false);
    }
});

$('button.sram-backup')?.addEventListener('click', async () => {
    await commandWithProgress('Sr\r', 'Backing up save to onboard storage...');
});

$('button.sram-restore')?.addEventListener('click', async () => {
    await commandWithProgress('Sw\r', 'Restoring previous save from onboard storage...');
});

$('button.sram-format-fs')?.addEventListener('click', async () => {
    // TODO confirmation
    await commandWithProgress('Sf\r', 'Erasing all backed-up saves...');
});

$('button.flash-download')?.addEventListener('click', async () => {
    try {
        assertConnected();
        setBusy(true);
        const header = await downloadAndParseCartHeader();
        assert(header.romSize > 0 && header.romSize <= FLASH_SIZE,
            `Header indicates invalid ROM size ${header.romSize}. Flash contents might be empty or invalid Loopy ROM.`);
        setStatus(`Dumping ${header.romSize} bytes...`);
        saveBufferToFile(await download(header.romSize, `D${header.romSize}\r`), 'loopy-rom.bin');
    } finally {
        setBusy(false);
    }
});

$('button.sram-download')?.addEventListener('click', async () => {
    try {
        assertConnected();
        setBusy(true);
        const header = await downloadAndParseCartHeader();
        const cart = lookupCartDatabase(header.checksum);

        // SRAM always downloads the whole thing, this is a good thing, we can truncate if desired
        let buffer = await download(SRAM_SIZE, `Ds\r`);
        if (cart && TRUNCATE_DUMPED_SRAM) {
            if (header.sramSize > 0 && header.sramSize < buffer.byteLength) {
                console.log(`truncating ${buffer.byteLength / 1024}kb downloaded to declared save size of ${header.sramSize / 1024}kb`)
                buffer = new Uint8Array(buffer, 0, header.sramSize);
            }
            saveBufferToFile(buffer, `${cart.name}.sav`);
        } else {
            saveBufferToFile(buffer, 'loopy.sav')
        }
    } finally {
        setBusy(false);
    }
});

$('button.flash-erase')?.addEventListener('click', async () => {
    await commandWithProgress('E\r', 'Erasing flash banks 0 and 1...');
});
$('button.flash-erase-one')?.addEventListener('click', async () => {
    await commandWithProgress('E0\r', 'Erasing flash bank 0...');
});
$('button.sram-erase')?.addEventListener('click', async () => {
    await commandWithProgress('Es\r', 'Initializing full contents of SRAM...');
});

$('.device-nickname button')?.addEventListener('click', async () => {
    assertConnected();
    const nick = $('.device-nickname input').value;
    await floopy.send(`N${nick}\r`);
});

$('form.manual-command')?.addEventListener('submit', async (event) => {
    event.preventDefault();
    assertConnected();
    const $input = $('.manual-command input');
    const cmd = `${$input.value.toUpperCase().trim()}\r`;
    $input.value = '';
    await floopy.send(cmd);
    return false;
});

$('button.cls')?.addEventListener('click', () => {
    cls();
});

window.addEventListener('unhandledrejection', rejectionEvent => {
    showError(rejectionEvent.reason).then();
});

window.addEventListener('error', errEvent => {
    showError(errEvent.error).then()
});
