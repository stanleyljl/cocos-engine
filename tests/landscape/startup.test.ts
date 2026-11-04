import { director } from '../../cocos/game/director';
import { Game } from '../../cocos/game/game';
import { SplashScreen } from '../../cocos/game/splash-screen';
import { Landscape } from '../../cocos/landscape/landscape';
import type { LandscapeInstance } from '../../cocos/landscape/LandscapeInstance';
import { RenderScene } from '../../cocos/render-scene/core/render-scene';
import { Node, Scene } from '../../cocos/scene-graph';

afterEach(() => jest.restoreAllMocks());

test('scene readiness waits for every terrain and pending initialization', () => {
    const scene = new RenderScene(director.root!);
    expect(scene.isReady()).toBe(true);
    scene.beginLandscapeInitialization();
    scene.beginLandscapeInitialization();
    const first = { isReady: jest.fn(() => true) } as unknown as LandscapeInstance;
    const second = { isReady: jest.fn(() => false) } as unknown as LandscapeInstance;
    scene.addLandscape(first);
    scene.endLandscapeInitialization();
    expect(scene.isReady()).toBe(false);
    scene.addLandscape(second);
    scene.endLandscapeInitialization();
    expect(scene.isReady()).toBe(false);
    jest.mocked(second.isReady).mockReturnValue(true);
    expect(scene.isReady()).toBe(true);
    scene.removeLandscape(first);
    scene.removeLandscape(second);
    expect(scene.isReady()).toBe(true);
});

test.each(['success', 'failure', 'cancel'] as const)('async initialization keeps splash pending: %s', async (outcome) => {
    const renderScene = new RenderScene(director.root!);
    const node = new Node();
    Object.defineProperty(node, 'scene', { value: { renderScene } });
    let complete!: (success: boolean) => void;
    const initialized = new Promise<boolean>((resolve) => {
        complete = resolve;
    });
    const runtime = {
        setAssetPath: jest.fn(),
        setHoleMode: jest.fn(),
        setDebugData: jest.fn(),
        setGlobalColorMap: jest.fn(),
        setGlobalColorStrength: jest.fn(),
        setCastShadow: jest.fn(),
        setReceiveShadow: jest.fn(),
        onEnable: jest.fn(() => initialized),
        onDisable: jest.fn(),
        isReady: jest.fn(() => false),
    };
    const terrain = new Landscape();
    Object.assign(terrain, { node, _runtime: runtime, _landscapeAsset: { manifestPath: 'terrain' } });
    Object.defineProperty(terrain, 'enabledInHierarchy', { get: () => true });
    terrain.onEnable();
    expect(renderScene.isReady()).toBe(false);
    if (outcome === 'cancel') {
        terrain.onDisable();
        expect(renderScene.isReady()).toBe(true);
    } else if (outcome === 'success') {
        // Successful runtime publication precedes the component's completion callback.
        renderScene.addLandscape(runtime as unknown as LandscapeInstance);
    }
    complete(outcome !== 'failure');
    await Promise.resolve();
    expect(renderScene.isReady()).toBe(outcome === 'cancel');
    if (outcome === 'success') {
        runtime.isReady.mockReturnValue(true);
        expect(renderScene.isReady()).toBe(true);
        renderScene.removeLandscape(runtime as unknown as LandscapeInstance);
    }
    terrain.onDisable();
    terrain.onDestroy();
    expect(renderScene.isReady()).toBe(true);
    node.destroy();
});

interface StartupGame {
    _inited: boolean;
    _shouldLoadLaunchScene: boolean;
    _calculateDT(fixed: boolean): number;
    _updateCallback(): void;
    _onAfterRender(): void;
}

function loadingGame () {
    const game = new Game() as unknown as StartupGame;
    game._inited = true;
    game._shouldLoadLaunchScene = false;
    game._calculateDT = () => 1 / 60;
    const splash = {
        enabled: true,
        isFinished: true,
        update: jest.fn(),
        draw: jest.fn(),
    } as unknown as SplashScreen;
    let active: SplashScreen | null = splash;
    jest.spyOn(SplashScreen, 'instance', 'get').mockImplementation(() => active);
    const release = jest.spyOn(SplashScreen, 'releaseInstance').mockImplementation(() => {
        active = null;
    });
    return { game, splash, release };
}

test('splash holds its last frame and releases only after a ready rendered frame', () => {
    const { game, splash, release } = loadingGame();
    const ready = jest.fn(() => false);
    jest.spyOn(director, 'getScene').mockReturnValue({ renderScene: { isReady: ready, cameras: [{ enabled: true }] } } as unknown as Scene);
    jest.spyOn(director, 'tick').mockImplementation(() => {
        game._onAfterRender();
        ready.mockReturnValue(true);
        game._onAfterRender();
    });
    game._updateCallback();
    expect(ready).toHaveBeenCalledTimes(1);
    expect(splash.draw).toHaveBeenNthCalledWith(1, 0);
    expect(splash.draw).toHaveBeenNthCalledWith(2, 1);
    expect(splash.update).not.toHaveBeenCalled();
    expect(release).not.toHaveBeenCalled();
    game._updateCallback();
    expect(splash.draw).toHaveBeenCalledTimes(2);
    expect(release).toHaveBeenCalledTimes(1);
    ready.mockReturnValue(false);
    game._updateCallback();
    expect(splash.draw).toHaveBeenCalledTimes(2);
    expect(release).toHaveBeenCalledTimes(1);
});

test('splash continues while the scene is still loading and updates continue', () => {
    const { game, splash, release } = loadingGame();
    jest.spyOn(director, 'getScene').mockReturnValue(null);
    const tick = jest.spyOn(director, 'tick').mockImplementation(() => {});
    game._updateCallback();
    game._updateCallback();
    expect(tick).toHaveBeenCalledTimes(2);
    expect(splash.update).toHaveBeenNthCalledWith(1, 0);
    expect(splash.update).toHaveBeenNthCalledWith(2, 0);
    expect(release).not.toHaveBeenCalled();
});

test.each([{ cameras: [] }, { cameras: [{ enabled: false }] }])('a loaded scene with no enabled camera releases splash without rendering: $cameras', ({ cameras }) => {
    const { game, splash, release } = loadingGame();
    const ready = jest.fn(() => false);
    jest.spyOn(director, 'getScene').mockReturnValue({ renderScene: { isReady: ready, cameras } } as unknown as Scene);
    const tick = jest.spyOn(director, 'tick').mockImplementation(() => {});
    game._updateCallback();
    game._updateCallback();
    expect(release).toHaveBeenCalledTimes(1);
    expect(tick).toHaveBeenCalledTimes(2);
    expect(ready).not.toHaveBeenCalled();
    expect(splash.draw).not.toHaveBeenCalled();
    expect(splash.update).not.toHaveBeenCalled();
});

test('a camera enabled during scene updates still waits for terrain readiness', () => {
    const { game, splash, release } = loadingGame();
    const camera = { enabled: false };
    const ready = jest.fn(() => false);
    jest.spyOn(director, 'getScene').mockReturnValue({ renderScene: { isReady: ready, cameras: [camera] } } as unknown as Scene);
    jest.spyOn(director, 'tick').mockImplementation(() => {
        camera.enabled = true;
        game._onAfterRender();
    });
    game._updateCallback();
    expect(splash.draw).toHaveBeenCalledWith(0);
    expect(release).not.toHaveBeenCalled();
    jest.mocked(director.tick).mockImplementation(() => {
        camera.enabled = false;
    });
    game._updateCallback();
    expect(release).toHaveBeenCalledTimes(1);
    expect(splash.update).not.toHaveBeenCalled();
});
