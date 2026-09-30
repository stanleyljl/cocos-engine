const result = document.getElementById('result');
try {
    const canvas = document.createElement('canvas'); canvas.width = canvas.height = 64;
    const gl = canvas.getContext('webgl2', { antialias: false });
    if (!gl) throw Error('WebGL2 unavailable');
    const check = (condition, message) => { if (!condition) throw Error(message); };
    const link = (vs, fs) => {
        const p = gl.createProgram();
        for (const [type, source] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type); gl.shaderSource(s, '#version 300 es\n' + source); gl.compileShader(s);
            check(gl.getShaderParameter(s, gl.COMPILE_STATUS), gl.getShaderInfoLog(s)); gl.attachShader(p, s);
        }
        gl.linkProgram(p); check(gl.getProgramParameter(p, gl.LINK_STATUS), gl.getProgramInfoLog(p)); return p;
    };
    let variants = 0;
    for (const shader of fixture.effect.shaders) for (const decal of [0, 1]) for (const wire of [0, 1]) for (const unorm of [0, 1]) {
        const defines = { USE_INSTANCING: 1, LANDSCAPE_DECAL_MESH: decal, LANDSCAPE_WIREFRAME: wire,
            LANDSCAPE_HEIGHT_UNORM: unorm, CC_RECEIVE_SHADOW: 1, CC_USE_FOG: 4 };
        const prefix = 'precision highp sampler2DArray;\n#define CC_DEVICE_SUPPORT_FLOAT_TEXTURE 0\n#define CC_PLATFORM_ANDROID_AND_WEBGL 0\n#define CC_ENABLE_WEBGL_HIGHP_STRUCT_VALUES 0\n'
            + shader.defines.map(d => '#define ' + d.name + ' ' + (defines[d.name] ?? d.default ?? d.range?.[0] ?? 0)).join('\n') + '\n';
        gl.deleteProgram(link(prefix + shader.glsl3.vert, prefix + shader.glsl3.frag)); variants++;
    }
    const common = 'precision highp float;precision highp int;precision highp sampler2DArray;\nuniform vec4 heightParams,holeParams;\n';
    const vertex = common + '#define USE_INSTANCING 1\n#define LANDSCAPE_DECAL_MESH 0\n#define LANDSCAPE_SHADOW_PASS 0\n#define LANDSCAPE_WIREFRAME 0\n#define LANDSCAPE_HEIGHT_UNORM 0\n'
        + 'uniform vec4 terrainParams,morphCameraPos,decalControl,lodMorph[9];\n'
        + 'struct SurfacesStandardVertexIntermediate {vec3 position;vec3 normal;vec4 tangent;vec2 texCoord;};\n'
        + fixture.vertex + '\nin vec2 position;void main(){SurfacesStandardVertexIntermediate v;v.position=vec3(position,0);'
        + 'vec4 p=mat4(1.0)*vec4(SurfacesVertexModifyLocalPos(v),1.0);gl_Position=vec4(p.x/8.0-1.0,p.z/8.0-1.0,p.y,p.w);}';
    const fragment = common + fixture.fragment + '\nout vec4 color;void main(){discardLandscapeHole();color=vec4(1);}';
    const p = link(vertex, fragment); gl.useProgram(p);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const grid = [];
    for (let z = 0; z < 16; z++) for (let x = 0; x < 16; x++) {
        grid.push(x/16,z/16,x/16,(z+1)/16,(x+1)/16,z/16,(x+1)/16,z/16,x/16,(z+1)/16,(x+1)/16,(z+1)/16);
    }
    gl.bindVertexArray(gl.createVertexArray()); gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(grid), gl.STATIC_DRAW);
    const location = gl.getAttribLocation(p, 'position'); gl.enableVertexAttribArray(location); gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    for (const [name, value] of Object.entries({ a_gridInst: [0,0,16,0], a_quadrantInst: [0,0,1,0], a_tileInst: [0,0,16,0], a_vtInst: [0,0,16,0] })) {
        const at = gl.getAttribLocation(p, name); if (at >= 0) gl.vertexAttrib4f(at, ...value);
    }
    const uniform = (name, ...v) => gl.uniform4f(gl.getUniformLocation(p, name), ...v);
    uniform('heightParams', 1,17,0,0); uniform('terrainParams', 1,0,0,0);
    const texture = (name, unit, format, type, channels, data) => {
        gl.activeTexture(gl.TEXTURE0 + unit); const t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D_ARRAY, t);
        gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, format, 17,17,1,0,channels,type,data);
        gl.uniform1i(gl.getUniformLocation(p, name), unit); return t;
    };
    texture('heightmap', 0, gl.RG8, gl.UNSIGNED_BYTE, gl.RG, new Uint8Array(17*17*2));
    const mask = new Uint16Array(17*17).fill(31 | (30<<5) | (31<<10));
    const holes = texture('holeSplatmap', 1, gl.R16UI, gl.UNSIGNED_SHORT, gl.RED_INTEGER, mask);
    const draw = mode => {
        gl.useProgram(p); uniform('holeParams', mode,0,0,0);
        gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D_ARRAY, holes);
        gl.texSubImage3D(gl.TEXTURE_2D_ARRAY,0,0,0,0,17,17,1,gl.RED_INTEGER,gl.UNSIGNED_SHORT,mask);
        gl.viewport(0,0,64,64); gl.clearColor(0,0,0,0); gl.clear(gl.COLOR_BUFFER_BIT); gl.drawArrays(gl.TRIANGLES,0,grid.length/2);
        const pixels = new Uint8Array(64*64*4); gl.readPixels(0,0,64,64,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
        check(gl.getError() === gl.NO_ERROR, 'draw/readback error'); return pixels;
    };
    const alpha = (pixels,x,z) => pixels[(z*64+x)*4+3];
    for (const mode of [0,1]) check(draw(mode).every(v=>v===255), 'material bits must not remove solid terrain');
    mask[8*17+8] |= 32768;
    const high = draw(1), fast = draw(0);
    for (let z=0;z<64;z++) for (let x=0;x<64;x++) {
        check(alpha(high,x,z) === (x>=32&&x<36&&z>=32&&z<36 ? 0 : 255), 'fragment hole cell boundary');
    }
    check(alpha(fast,31,31)===0 && alpha(fast,33,33)===0, 'vertex hole removes incident triangles');
    check(alpha(fast,10,10)===255, 'vertex hole preserves distant triangles');
    const layer = gl.getAttribLocation(p,'a_tileInst');
    gl.vertexAttrib4f(layer,0,0,32,0); // Same streamed samples at a coarser source LOD.
    mask.fill(0); mask[4*17+4]=32768;
    const coarse = draw(1);
    check(alpha(coarse,33,33)===0 && alpha(coarse,39,39)===0 && alpha(coarse,40,40)===255, 'coarse source changes cell size');
    result.textContent = `PASS: ${variants} production shader variants linked; material/hole separation, exact fragment cells, vertex NaN triangle removal and source LOD checked.`;
} catch (e) { result.textContent = 'FAIL: ' + e.stack; }
