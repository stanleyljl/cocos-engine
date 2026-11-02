/*
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 https://www.cocos.com/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

import type { Scene } from '../scene-graph/scene';
import { EDITOR_NOT_IN_PREVIEW } from 'internal:constants';
import { director, DirectorEvent } from '../game/director';
import { Color, Rect } from '../gfx';
import { cclegacy } from '../core/global-exports';

const loadingClearColors = [new Color(0, 0, 0, 1)];
const loadingRenderArea = new Rect();

/**
 * Landscape owns startup waiting and screen coverage on both Web and JSB.
 * Normal rendering keeps preparing terrain behind a black frame. Components
 * and scene-launch callbacks keep their normal lifecycle; later streaming never
 * hides an already presented scene.
 */
export class LandscapeStartup {
    // Weak keys keep presentation bookkeeping out of Scene and its native lifetime.
    // Components cancel their pending registrations on disable/destruction.
    private static readonly _scenes = new WeakMap<Scene, LandscapeStartup>();
    private static _eye = 0;

    public static forScene (scene: Scene): LandscapeStartup {
        let presentation = this._scenes.get(scene);
        if (!presentation) {
            presentation = new LandscapeStartup();
            this._scenes.set(scene, presentation);
        }
        return presentation;
    }

    private readonly _pending = new Set<{ isReady: () => boolean }>();
    private _committed = false;

    /**
     * Register synchronously before starting asynchronous preparation. The predicate
     * must stay false on failure; the owner reports the error. Cancel on disable or
     * destruction. Registrations after first presentation do not hide the scene again.
     */
    public deferUntilReady (isReady: () => boolean): () => void {
        const pending = { isReady };
        if (!this._committed) {
            this._pending.add(pending);
        }
        return (): void => {
            this._pending.delete(pending);
        };
    }

    /** Evaluate once before presenting the frame, so every camera uses the same decision. */
    private tryCommit (): void {
        if (!this._committed) {
            for (const pending of this._pending) {
                if (!pending.isReady()) {
                    return;
                }
            }
            this._committed = true;
            this._pending.clear();
        }
    }

    public static initialize (): void {
        if (!EDITOR_NOT_IN_PREVIEW) {
            director.on(DirectorEvent.BEFORE_DRAW, this.beginFrame, this);
            director.on(DirectorEvent.BEFORE_PRESENT, this.present, this);
        }
    }

    private static beginFrame (): void {
        this._eye = 0;
    }

    private static present (): void {
        const eye = this._eye++;
        const scene = director.getScene();
        if (!scene) {
            return;
        }
        // Record first presentation even without a Landscape, so adding one to an
        // already visible scene uses normal streaming instead of hiding that scene.
        const startup = this.forScene(scene);
        if (eye === 0) {
            startup.tryCommit();
        }
        if (startup._committed) {
            return;
        }
        this.clearFrame();
    }

    private static clearFrame (): void {
        const root = director.root!;
        const device = root.device;
        const cmdBuff = device.commandBuffer;
        device.enableAutoBarrier(true);
        cmdBuff.begin();
        for (const window of root.windows) {
            if (!window.swapchain) {
                continue;
            }
            const framebuffer = window.framebuffer;
            loadingRenderArea.width = window.swapchain.width;
            loadingRenderArea.height = window.swapchain.height;
            cmdBuff.beginRenderPass(framebuffer.renderPass, framebuffer, loadingRenderArea, loadingClearColors, 1, 0);
            cmdBuff.endRenderPass();
        }
        cmdBuff.end();
        device.flushCommands([cmdBuff]);
        device.queue.submit([cmdBuff]);
        device.enableAutoBarrier(!cclegacy.rendering);
    }
}

LandscapeStartup.initialize();
