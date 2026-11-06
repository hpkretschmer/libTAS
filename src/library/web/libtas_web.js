//=============================================================================
// libtas_web.js
//=============================================================================
// Makes a web game (NW.js) deterministic under libTAS, without knowing anything
// about the game. libTAS (with LIBTAS_PAGE_FRAMES=1) injects it into NW.js games
// before any script of the game runs, with "inject_js_start" in the manifest
// package.json that it gives to NW.js (see PageScript.h), so the game's files
// stay unchanged. It is built into libtas.so, so it always matches libTAS.
//
// It does the following:
// - Frames: each animation frame (requestAnimationFrame) of the page is one
//   libTAS frame, signaled to libTAS. The browser presents images on its own
//   schedule, so libTAS cannot tell game frames from other presents by itself.
// - Time: the page's clock (performance.now(), Date.now() and the time passed
//   to animation frame callbacks) advances exactly one frame duration per
//   frame, at the framerate set in libTAS (1/60 s at 60 fps).
// - Inputs: the inputs of each frame are read from libTAS and sent to the page
//   as keyboard and mouse events at the start of the frame. The real input
//   events, which arrive at an undefined time, are ignored.
// - Loading: while images, XMLHttpRequests, fetches, audio decoding or Node.js
//   file and zlib operations (NW.js) are in progress, the page's animation
//   frames are held back, so that loading takes no frames, as if loading was
//   instant.
// - Fast-forward: when libTAS skips drawing a frame, the page does not draw on
//   the canvases shown on screen, and the frame runs right after the previous
//   one and ends without waiting for the browser to present it.
// - Non-draw frames: frames in which the page does not draw on screen are
//   non-draw frames in libTAS, also when fast-forwarding.
//
// Observers for debugging can set window.__libtasAfterFrame to a function,
// which is called with the frame number after each frame.
//=============================================================================

(function() {
    /* Use the real node fs (mod loaders such as OneLoader replace the fs
     * module of the page) */
    var fs = null;
    try {
        fs = process.mainModule.require('fs');
    } catch (e) {}

    /* Signals a frame with readlink() and returns its inputs (see PageFrame.h
     * in libTAS), or null if that failed */
    function readlinkSignal() {
        try {
            return fs.readlinkSync('/libtas/page_frame');
        } catch (e) {
            return null;
        }
    }

    /* If the signal fails, e.g. without page frame support, the page stays
     * unchanged */
    if (!fs || (readlinkSignal() === null))
        return;

    //-------------------------------------------------------------------------
    // Inputs
    //-------------------------------------------------------------------------

    /* X keysyms used by libTAS to browser key codes */
    var keyCodes = {
        0xff08: 8, 0xff09: 9, 0xff0d: 13, 0xff13: 19, 0xff1b: 27, 0x20: 32,
        0xff55: 33, 0xff56: 34, 0xff57: 35, 0xff50: 36,
        0xff51: 37, 0xff52: 38, 0xff53: 39, 0xff54: 40, 0xff63: 45, 0xffff: 46,
        0xffe1: 16, 0xffe2: 16, 0xffe3: 17, 0xffe4: 17, 0xffe9: 18, 0xffea: 18,
        0xffe5: 20, 0xff7f: 144, 0xff14: 145, 0xff8d: 13,
        0xffaa: 106, 0xffab: 107, 0xffad: 109, 0xffae: 110, 0xffaf: 111,
        0x3b: 186, 0x3d: 187, 0x2c: 188, 0x2d: 189, 0x2e: 190, 0x2f: 191,
        0x60: 192, 0x5b: 219, 0x5c: 220, 0x5d: 221, 0x27: 222
    };
    var k;
    for (k = 0; k < 26; k++) {
        keyCodes[0x61 + k] = 65 + k;
        keyCodes[0x41 + k] = 65 + k;
    }
    for (k = 0; k < 10; k++) {
        keyCodes[0x30 + k] = 48 + k;
        keyCodes[0xffb0 + k] = 96 + k;
    }
    for (k = 0; k < 12; k++) {
        keyCodes[0xffbe + k] = 112 + k;
    }

    /* libTAS pointer button bits to browser buttons */
    var mouseButtons = [
        {bit: 1, button: 0, buttons: 1},
        {bit: 2, button: 1, buttons: 4},
        {bit: 4, button: 2, buttons: 2}
    ];

    /* Inputs last sent to the page */
    var sentKeys = {};
    var sentMouse = {x: 0, y: 0, mask: 0};

    /* Ignore the real input events, the page gets them from libTAS */
    ['keydown', 'keyup', 'keypress', 'mousedown', 'mouseup', 'mousemove',
     'click', 'dblclick', 'wheel'].forEach(function(type) {
        window.addEventListener(type, function(event) {
            if (event.isTrusted)
                event.stopImmediatePropagation();
        }, true);
    });

    function sendKey(type, keyCode, keys) {
        var event = new KeyboardEvent(type, {
            bubbles: true, cancelable: true,
            shiftKey: !!keys[16], ctrlKey: !!keys[17], altKey: !!keys[18]
        });
        Object.defineProperty(event, 'keyCode', {value: keyCode});
        Object.defineProperty(event, 'which', {value: keyCode});
        document.dispatchEvent(event);
    }

    function sendMouse(type, x, y, button, mask, keys) {
        var buttons = 0;
        mouseButtons.forEach(function(b) {
            if (mask & b.bit)
                buttons |= b.buttons;
        });
        document.dispatchEvent(new MouseEvent(type, {
            bubbles: true, cancelable: true,
            clientX: x, clientY: y, screenX: x, screenY: y,
            button: button, buttons: buttons,
            shiftKey: !!keys[16], ctrlKey: !!keys[17], altKey: !!keys[18]
        }));
    }

    /* Send the changes of the inputs from libTAS as events to the page. The
     * inputs are "<pointer x> <pointer y> <button mask> <wheel> r<num>/<den>
     * [s] <keysym>...", see PageFrame.h in libTAS. Returns the framerate field
     * "r<num>/<den>", and if the frame skips its draw ("s", when
     * fast-forwarding). */
    function applyInputs(inputs) {
        var fields = inputs.split(' ');
        var framerate;
        if (fields[4] && fields[4][0] === 'r')
            framerate = fields.splice(4, 1)[0];
        var skip = fields[4] === 's';
        if (skip)
            fields.splice(4, 1);
        var x = parseInt(fields[0], 10);
        var y = parseInt(fields[1], 10);
        var mask = parseInt(fields[2], 10);
        var wheel = parseInt(fields[3], 10);
        var keys = {};
        fields.slice(4).forEach(function(keysym) {
            var keyCode = keyCodes[parseInt(keysym, 16)];
            if (keyCode)
                keys[keyCode] = true;
        });

        Object.keys(sentKeys).forEach(function(keyCode) {
            if (!keys[keyCode])
                sendKey('keyup', Number(keyCode), keys);
        });
        Object.keys(keys).forEach(function(keyCode) {
            if (!sentKeys[keyCode])
                sendKey('keydown', Number(keyCode), keys);
        });
        sentKeys = keys;

        if (x !== sentMouse.x || y !== sentMouse.y)
            sendMouse('mousemove', x, y, 0, mask, keys);
        mouseButtons.forEach(function(b) {
            if ((mask & b.bit) && !(sentMouse.mask & b.bit))
                sendMouse('mousedown', x, y, b.button, mask, keys);
            if (!(mask & b.bit) && (sentMouse.mask & b.bit))
                sendMouse('mouseup', x, y, b.button, mask, keys);
        });
        if (wheel) {
            document.dispatchEvent(new WheelEvent('wheel', {
                bubbles: true, cancelable: true,
                clientX: x, clientY: y, deltaY: -100 * wheel
            }));
        }
        sentMouse = {x: x, y: y, mask: mask};
        return {framerate: framerate, skip: skip};
    }

    //-------------------------------------------------------------------------
    // Time
    //-------------------------------------------------------------------------

    var frames = 0;

    /* Duration of a frame in ms at a framerate of num/den, as a multiple of
     * 2^-24 ms, slightly longer than the exact duration (by 0.5 to 1.5 times
     * 2^-24 ms, about 6e-8 ms). Games accumulate the elapsed time and run one
     * logic step each time it reaches the step duration, e.g. 1/60 s. With
     * the exact duration, rounding errors put the accumulated time sometimes
     * just below the step duration and sometimes just above, so that some
     * frames run no logic step and others two. Multiples of 2^-24 ms add up
     * without rounding errors, and the small excess puts the accumulated time
     * always just above the step duration. It adds up to one extra logic step
     * only after more than 10^8 frames. */
    var scale = 16777216;
    function durationOf(num, den) {
        return Math.ceil(1000 * den / num * scale + 0.5) / scale;
    }
    var frameDuration = durationOf(60, 1);
    var framerate = 'r60/1';

    function setFramerate(field) {
        if (!field || field === framerate)
            return;
        var parts = field.slice(1).split('/');
        var num = parseInt(parts[0], 10);
        var den = parseInt(parts[1], 10);
        if (num > 0 && den > 0) {
            framerate = field;
            frameDuration = durationOf(num, den);
        }
    }

    /* Fixed start values, because the time when the page loads varies. The
     * date is rounded to the minute, libTAS starts at a fixed date. */
    var clockStart = 1000;
    var clock = clockStart;
    var dateStart = Math.floor(Date.now() / 60000) * 60000;

    function now() {
        return clock;
    }
    performance.now = now;
    Date.now = function() {
        return Math.floor(dateStart + (clock - clockStart));
    };

    //-------------------------------------------------------------------------
    // Loading
    //-------------------------------------------------------------------------

    /* Number of resource loads in progress */
    var pendingLoads = 0;

    /* Count a load of `target` until one of `endEvents` */
    var tracked = new WeakSet();
    function track(target, endEvents) {
        if (tracked.has(target))
            return;
        tracked.add(target);
        pendingLoads++;
        function end() {
            tracked.delete(target);
            pendingLoads--;
            endEvents.forEach(function(type) {
                target.removeEventListener(type, end);
            });
        }
        endEvents.forEach(function(type) {
            target.addEventListener(type, end);
        });
    }

    function trackPromise(promise) {
        pendingLoads++;
        var end = function() { pendingLoads--; };
        promise.then(end, end);
        return promise;
    }

    var imageSrc = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'src');
    Object.defineProperty(HTMLImageElement.prototype, 'src', {
        get: imageSrc.get,
        set: function(value) {
            if (value)
                track(this, ['load', 'error']);
            imageSrc.set.call(this, value);
        },
        configurable: true, enumerable: true
    });

    var xhrSend = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.send = function() {
        track(this, ['loadend']);
        return xhrSend.apply(this, arguments);
    };

    if (window.fetch) {
        var fetchOrig = window.fetch;
        window.fetch = function() {
            return trackPromise(fetchOrig.apply(this, arguments));
        };
    }

    /* Node.js operations with a callback (NW.js), e.g. the file reads of mod
     * loaders such as OneLoader. They run on other threads and finish after a
     * time that varies, so they are loads too. The callback is counted until
     * it returned, so that loads it starts (also in promise reactions, which
     * run right after it) are counted before the next frame. */
    function trackCallbacks(module, names) {
        if (!module)
            return;
        names.forEach(function(name) {
            var orig = module[name];
            if (typeof orig !== 'function')
                return;
            var wrapped = function() {
                var last = arguments.length - 1;
                var callback = arguments[last];
                if (typeof callback !== 'function')
                    return orig.apply(this, arguments);
                var args = Array.prototype.slice.call(arguments);
                var finished = false;
                args[last] = function() {
                    if (finished)
                        return callback.apply(this, arguments);
                    finished = true;
                    try {
                        return callback.apply(this, arguments);
                    } finally {
                        pendingLoads--;
                    }
                };
                pendingLoads++;
                try {
                    return orig.apply(this, args);
                } catch (e) {
                    if (!finished) {
                        finished = true;
                        pendingLoads--;
                    }
                    throw e;
                }
            };
            /* Keep properties such as util.promisify.custom or realpath.native */
            Object.getOwnPropertyNames(orig).concat(Object.getOwnPropertySymbols(orig)).forEach(function(key) {
                if (['length', 'name', 'prototype', 'arguments', 'caller'].indexOf(key) === -1)
                    Object.defineProperty(wrapped, key, Object.getOwnPropertyDescriptor(orig, key));
            });
            module[name] = wrapped;
        });
    }

    /* Node.js streams that work on other threads, until their work is done */
    function trackStreams(module, names) {
        if (!module)
            return;
        names.forEach(function(name) {
            var orig = module[name];
            if (typeof orig !== 'function')
                return;
            module[name] = function() {
                var stream = orig.apply(this, arguments);
                if (!stream || !stream.once)
                    return stream;
                var ended = false;
                var end = function() {
                    if (!ended) {
                        ended = true;
                        pendingLoads--;
                    }
                };
                pendingLoads++;
                ['finish', 'close', 'error'].forEach(function(type) {
                    stream.once(type, end);
                });
                return stream;
            };
        });
    }

    function nodeModule(name) {
        try {
            return process.mainModule.require(name);
        } catch (e) {
            return null;
        }
    }

    trackCallbacks(fs, ['access', 'appendFile', 'chmod', 'chown', 'close', 'copyFile',
        'exists', 'fchmod', 'fchown', 'fdatasync', 'fstat', 'fsync', 'ftruncate',
        'futimes', 'lchmod', 'lchown', 'link', 'lstat', 'mkdir', 'mkdtemp', 'open',
        'read', 'readdir', 'readFile', 'readlink', 'realpath', 'rename', 'rmdir',
        'stat', 'symlink', 'truncate', 'unlink', 'utimes', 'write', 'writeFile']);
    var zlib = nodeModule('zlib');
    trackCallbacks(zlib, ['deflate', 'deflateRaw', 'gzip', 'gunzip', 'inflate',
        'inflateRaw', 'unzip', 'brotliCompress', 'brotliDecompress']);
    trackStreams(zlib, ['createDeflate', 'createDeflateRaw', 'createGzip',
        'createGunzip', 'createInflate', 'createInflateRaw', 'createUnzip',
        'createBrotliCompress', 'createBrotliDecompress']);

    var AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (AudioContextClass) {
        var decode = AudioContextClass.prototype.decodeAudioData;
        AudioContextClass.prototype.decodeAudioData = function(data, success, error) {
            pendingLoads++;
            var finished = false;
            function finish() {
                if (!finished) {
                    finished = true;
                    pendingLoads--;
                }
            }
            var promise = decode.call(this, data,
                function(buffer) { finish(); if (success) success(buffer); },
                function(e) { finish(); if (error) error(e); });
            if (promise && promise.then)
                promise.then(finish, finish);
            return promise;
        };
    }

    //-------------------------------------------------------------------------
    // Skipped draws
    //-------------------------------------------------------------------------

    /* True if libTAS skips drawing the current frame (fast-forward) */
    var skipDraw = false;

    /* True if the page does not draw on the canvases shown on screen in the
     * current frame. Other canvases, e.g. images that the game draws text on,
     * are drawn as usual, because the game may use them in later frames.
     * Every 30th frame that skips its draw still draws on screen, so that the
     * screen in a savestate made while fast-forwarding is at most that old. */
    var skipScreenDraw = false;
    var screenDrawInterval = 30;

    /* True if the page drew on screen in the current frame, or would have if
     * the frame did not skip its draw. libTAS marks frames without drawing as
     * non-draw frames. */
    var drewOnScreen = false;

    var context2D = window.CanvasRenderingContext2D && CanvasRenderingContext2D.prototype;
    if (context2D) {
        ['drawImage', 'fillRect', 'clearRect', 'strokeRect', 'fillText',
         'strokeText', 'fill', 'stroke', 'putImageData'].forEach(function(name) {
            var orig = context2D[name];
            if (typeof orig !== 'function')
                return;
            context2D[name] = function() {
                if (this.canvas && this.canvas.isConnected) {
                    drewOnScreen = true;
                    if (skipScreenDraw)
                        return;
                }
                return orig.apply(this, arguments);
            };
        });
    }

    /* WebGL drawing is only noticed, not skipped */
    [window.WebGLRenderingContext, window.WebGL2RenderingContext].forEach(function(contextClass) {
        if (!contextClass)
            return;
        ['clear', 'drawArrays', 'drawElements', 'drawArraysInstanced',
         'drawElementsInstanced', 'drawRangeElements'].forEach(function(name) {
            var orig = contextClass.prototype[name];
            if (typeof orig !== 'function')
                return;
            contextClass.prototype[name] = function() {
                if (this.canvas && this.canvas.isConnected)
                    drewOnScreen = true;
                return orig.apply(this, arguments);
            };
        });
    });

    //-------------------------------------------------------------------------
    // Frames
    //-------------------------------------------------------------------------

    /* The browser only presents an image when something changed on screen, so
     * change an invisible pixel every frame to get one present per frame */
    var marker = null;
    var markerState = false;

    function toggleMarker() {
        if (!marker && document.body) {
            marker = document.createElement('div');
            marker.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0.01;z-index:2147483647;pointer-events:none;';
            document.body.appendChild(marker);
        }
        if (marker) {
            markerState = !markerState;
            marker.style.backgroundColor = markerState ? '#000' : '#010101';
        }
    }

    /* The page's animation frame callbacks run once per libTAS frame, and only
     * when no resource is loading */
    var raf = window.requestAnimationFrame.bind(window);
    var callbacks = [];
    var nextId = 1;
    var requested = false;
    var heldTicks = 0;

    /* Frames that skip their draw don't wait for the browser's next animation
     * frame, which comes at the browser's frame rate, but run right after the
     * previous frame. They end without a present, see the end of tick(). The
     * browser may still hold the message back until its next animation frame
     * (e.g. while loading), so request both, and run the frame with the one
     * that comes first. */
    var channel = new MessageChannel();
    var requestId = 0;

    /* True if the last frame drew outside of an animation frame, i.e. the
     * first frame after frames that skip their draw. The browser only commits
     * its drawing for a present at the end of its next animation frame, and a
     * frame waits at its start for the present of the previous one. So that
     * animation frame only commits, and the frame runs in the next one. */
    var drewOutsideAnimationFrame = false;

    function tickFor(id, inAnimationFrame) {
        if (!requested || (id !== requestId))
            return;
        if (inAnimationFrame && drewOutsideAnimationFrame) {
            drewOutsideAnimationFrame = false;
            raf(function() { tickFor(id, true); });
            return;
        }
        tick(inAnimationFrame);
    }
    channel.port1.onmessage = function(event) { tickFor(event.data, false); };

    function request(useAnimationFrame) {
        if (!requested) {
            requested = true;
            var id = ++requestId;
            if (skipDraw && !useAnimationFrame)
                channel.port2.postMessage(id);
            raf(function() { tickFor(id, true); });
        }
    }

    function tick(inAnimationFrame) {
        requested = false;
        if (pendingLoads > 0) {
            /* About 10 s at 60 fps */
            if (++heldTicks === 600)
                console.warn('libtas_web.js: frames held back for a long time, ' + pendingLoads + ' loads did not finish');
            /* Wait for the loads with animation frames, not busy waiting */
            request(true);
            return;
        }
        heldTicks = 0;

        var inputs = readlinkSignal();
        if (inputs !== null) {
            var frame = applyInputs(inputs);
            setFramerate(frame.framerate);
            skipDraw = frame.skip;
        }
        skipScreenDraw = skipDraw && ((frames % screenDrawInterval) !== 0);
        drewOnScreen = false;
        /* Also when the frame skips its draw: the browser's clock, which its
         * timers and loads use, only advances while it keeps presenting */
        toggleMarker();
        frames++;
        clock += frameDuration;

        var current = callbacks;
        callbacks = [];
        var time = now();
        current.forEach(function(entry) {
            try {
                entry.callback(time);
            } catch (e) {
                /* Report it like an exception in a native callback */
                setTimeout(function() { throw e; });
            }
        });

        /* Tell libTAS that the frame ended, and if it drew. A frame that
         * skips its draw or did not draw then ends without a present. */
        try {
            fs.accessSync(drewOnScreen ? '/libtas/page_frame_end' : '/libtas/page_frame_end_nodraw');
        } catch (e) {}

        drewOutsideAnimationFrame = !skipDraw && drewOnScreen && !inAnimationFrame;

        if (window.__libtasAfterFrame)
            window.__libtasAfterFrame(frames);
    }

    window.requestAnimationFrame = function(callback) {
        var id = nextId++;
        callbacks.push({id: id, callback: callback});
        request();
        return id;
    };
    window.cancelAnimationFrame = function(id) {
        callbacks = callbacks.filter(function(entry) {
            return entry.id !== id;
        });
    };
})();
