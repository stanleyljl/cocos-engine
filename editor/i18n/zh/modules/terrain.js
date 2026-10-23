module.exports = {
    classes: {
        cc: {
            Landscape: {
                properties: {
                    landscapeAsset: {
                        tooltip: '地形资源，包含地形尺寸、图层及瓦片数据。运行时按需加载瓦片。',
                    },
                    lodQualityScale: {
                        tooltip: '几何 LOD 精度倍率，默认值和最小值均为 1。值越大，较远处保留的几何细节越多；仅能在地形初始化前设置，不控制 Query 或物理精度。',
                    },
                    queryCacheCapacity: {
                        tooltip: 'CPU 表面查询缓存的瓦片数量上限，默认 64，范围 1～4096；仅能在地形初始化前设置。查询区域及其他查询源占用的瓦片超出容量时返回 OutOfCapacity。独立于几何、VT 和物理缓存。',
                    },
                    shadowCastingModeForInspector: {
                        tooltip: '控制地形及其立体贴花是否投射实时阴影。光源和渲染管线还需开启相应阴影功能。',
                    },
                    receiveShadowForInspector: {
                        tooltip: '控制地形及其立体贴花是否接收实时阴影。',
                    },
                    debugData: {
                        tooltip: '地形调试及渲染开关，包含线框、LOD、虚拟纹理和贴花显示选项。',
                    },
                    holeMode: {
                        tooltip: 'Fast 模式通过顶点 NaN 剔除三角形；HighQuality 模式在片元阶段裁剪。两者均跟随源数据 LOD。默认使用 Fast，需要时可手动选择 HighQuality。',
                    },
                    globalColorMap: {
                        tooltip: '覆盖整个地形的颜色调制图。RGB 为线性 UNORM 数据：字节值 128 不改变颜色，低于 128 压暗，高于 128 提亮。Alpha 控制区域影响程度。不要直接使用漫反射图片或 sRGB 纹理；未指定时保留原材质颜色。',
                    },
                    globalColorStrength: {
                        tooltip: '全局调制强度，与图像 Alpha 及各材质的 globalColorInfluence 相乘。0 关闭调制，默认 0.1。',
                    },
                },
            },
            LandscapeDebugData: {
                properties: {
                    wireframe: {
                        tooltip: '以线框显示地形和立体贴花网格，便于检查几何细分。',
                    },
                    lodColor: {
                        tooltip: '按地形几何 LOD 层级着色，便于观察细节层级分布和切换。',
                    },
                    showBox: {
                        tooltip: '绘制主相机当前视野内选中四叉树节点的包围盒。',
                    },
                    unlit: {
                        tooltip: '以无光照方式显示地形及立体贴花材质，便于检查表面颜色。',
                    },
                    showVTAtlas: {
                        tooltip: 'VT 图集预览开关。需由项目调试界面读取此开关，并使用 getVTAtlas() 显示图集。',
                    },
                    showRanges: {
                        tooltip: '显示 LOD 距离范围和 Sector 边界。',
                    },
                    heightBlendEnabled: {
                        tooltip: '开启高度混合；关闭使用线性权重混合。切换后重新生成 VT。',
                    },
                    refineFarDetail: {
                        tooltip: 'VT 层级 6 及以上使用精细地形法线；关闭时远近均使用基础精度。法线始终烘焙进 VT。',
                    },
                    decal3DEnabled: {
                        tooltip: '显示近景立体贴花网格；关闭后保留平面 RVT 贴花。',
                    },
                    physicsWireframe: {
                        tooltip: '显示已加载的地形碰撞网格，由项目物理调试视图读取此开关并绘制线框。',
                    },
                },
            },
            Terrain: {
                properties: {
                    _asset: {
                        tooltip: '地形所使用的资源。',
                    },
                    effectAsset: {
                        tooltip: '地形特效资源。',
                    },
                    lightmapInfos: {
                        tooltip: '地形信息。',
                    },
                    receiveShadow: {
                        tooltip: '是否接受阴影。',
                    },
                    useNormalMap: {
                        tooltip: '是否使用法线贴图。',
                    },
                    usePBR: {
                        tooltip: '是否使用物理材质。',
                    },
                    lodEnable: {
                        tooltip: '是否允许 lod。',
                    },
                    LodBias: {
                        tooltip: 'Lod 偏移距离。',
                    },
                },
            },
        },
    },
};
