import { CachedArray } from '../../core';
import { TextureBase } from '../../asset/assets/texture-base';
import { Device, Attribute } from '../../gfx';
import { Camera } from '../../render-scene/scene/camera';
import { Model } from '../../render-scene/scene/model';
import { SpriteFrame } from '../assets/sprite-frame';
import { UIStaticBatch } from '../components/ui-static-batch';
import { UIRenderer, RenderRoot2D } from '../framework';
import { StaticVBAccessor } from './static-vb-accessor';
import { DrawBatch2D } from './draw-batch';
import { BaseRenderData } from './render-data';
import { UIMeshRenderer } from '../components/ui-mesh-renderer';
import { Material } from '../../asset/assets';
import { Node } from '../../scene-graph';

export interface IBatcher {
    currBufferAccessor: StaticVBAccessor;
    readonly batches: CachedArray<DrawBatch2D>;
    // registerCustomBuffer (attributes: MeshBuffer | Attribute[], callback: ((...args: number[]) => void) | null) : MeshBuffer;
    // unRegisterCustomBuffer (buffer: MeshBuffer);

    currStaticRoot: UIStaticBatch | null;
    currIsStatic: boolean;

    device: Device;

    initialize(): boolean;
    destroy();

    addScreen (comp: RenderRoot2D);
    getFirstRenderCamera (node: Node): Camera | null;
    removeScreen (comp: RenderRoot2D);

    sortScreens ();

    update ();
    uploadBuffers ();
    reset ();

    switchBufferAccessor (attributes?: Attribute[]): StaticVBAccessor;

    commitComp (comp: UIRenderer, renderData: BaseRenderData|null, frame: TextureBase | SpriteFrame | null, assembler: any, transform: Node | null);
    commitModel (comp: UIMeshRenderer | UIRenderer, model: Model | null, mat: Material | null);

    setupStaticBatch (staticComp: UIStaticBatch, bufferAccessor: StaticVBAccessor);
    endStaticBatch ();
    commitStaticBatch (comp: UIStaticBatch);

    autoMergeBatches (renderComp?: UIRenderer);
    forceMergeBatches (material: Material, frame: TextureBase | SpriteFrame | null, renderComp: UIRenderer);
    finishMergeBatches ();
    flushMaterial (mat: Material);

    walk (node: Node, level?: number);
}
