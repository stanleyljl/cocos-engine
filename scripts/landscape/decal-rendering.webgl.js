// Browser-side GPU regression harness; decal-rendering.test.js injects the compiled effect.
const result = document.getElementById('result');
try {
  const gl = document.createElement('canvas').getContext('webgl2');
  if (!gl) throw new Error('WebGL2 unavailable');
  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, '#version 300 es\n' + source); gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  const programs = effect.shaders.map(s => {
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl.VERTEX_SHADER, s.glsl3.vert));
    gl.attachShader(p, compile(gl.FRAGMENT_SHADER, s.glsl3.frag)); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  });
  const p = programs[0]; gl.useProgram(p);
  const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
  const buffer = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,0,1,0,0,1,0,1,1,0,1,1]), gl.STATIC_DRAW);
  const pos = gl.getAttribLocation(p,'a_position'); gl.enableVertexAttribArray(pos); gl.vertexAttribPointer(pos,2,gl.FLOAT,false,0,0);
  gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtPage'),0,0,0,0);
  gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtSource'),0,0,1,0);
  const region = gl.getAttribLocation(p,'a_vtRegion');
  const ubo = gl.createBuffer(); gl.bindBufferBase(gl.UNIFORM_BUFFER,0,ubo);
  gl.uniformBlockBinding(p,gl.getUniformBlockIndex(p,'Constants'),0);
  const constants = new Float32Array(692);
  constants.set([1,1,0,1],0); constants.set([2,0,0,0],4);
  constants.set([1,1,0,0],136); constants.set([1,3,2,0],140);
  const tex = (name,unit,target) => {
    gl.activeTexture(gl.TEXTURE0+unit); const t=gl.createTexture(); gl.bindTexture(target,t);
    gl.texParameteri(target,gl.TEXTURE_MIN_FILTER,gl.NEAREST); gl.texParameteri(target,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
    gl.texParameteri(target,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE); gl.texParameteri(target,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
    gl.uniform1i(gl.getUniformLocation(p,name),unit); return t;
  };
  const splat=tex('splatmap',0,gl.TEXTURE_2D_ARRAY);
  const colors=Array.from({length:8},(_,i)=>[32+i*27,220-i*23,50+i*17,i%2?255:0]);
  const normals=Array.from({length:8},(_,i)=>[128,128,20+i*25,230-i*20]);
  const ah=tex('albedoHeightMap',1,gl.TEXTURE_2D_ARRAY); gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(colors.flat()));
  const nra=tex('normalRoughnessAOMap',2,gl.TEXTURE_2D_ARRAY); gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(normals.flat()));
  tex('globalColorMap',3,gl.TEXTURE_2D); gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([255,255,255,255]));
  const decalColor=tex('decalAlbedoMap',4,gl.TEXTURE_2D_ARRAY);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([50,25,10,128]));
  tex('decalNormalMap',5,gl.TEXTURE_2D_ARRAY);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([190,90,128,255]));
  const decalIndices=tex('decalIndexMap',9,gl.TEXTURE_2D);
  const indexBytes=Uint8Array.from({length:128*4},(_,i)=>i%128);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,32,4,0,gl.RGBA,gl.UNSIGNED_BYTE,indexBytes);
  const fb=gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER,fb);
  gl.activeTexture(gl.TEXTURE0+6);
  for(let i=0;i<2;i++) { const t=gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D,t); gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,2,2,0,gl.RGBA,gl.UNSIGNED_BYTE,null); gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0+i,gl.TEXTURE_2D,t,0); }
  gl.drawBuffers([gl.COLOR_ATTACHMENT0,gl.COLOR_ATTACHMENT1]);
  if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE) throw new Error('Incomplete framebuffer');
  gl.viewport(0,0,1,1);
  const pack=([a,b,w])=>a|(b<<5)|(w<<10);
  const render=(pairs,{strength=1,scale=1,x=.5,y=.5,span=1,atlasCount=1,slot=0,sourceX=0,sourceY=0,sourceSize=1,sourceLayer=0}={})=>{
    constants[0]=atlasCount;gl.viewport(0,0,atlasCount,atlasCount);
    gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtPage'),slot,constants[656],0,0);
    gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtSource'),sourceX,sourceY,sourceSize,sourceLayer);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY,splat);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.R16UI,2,2,1,0,gl.RED_INTEGER,gl.UNSIGNED_SHORT,new Uint16Array(pairs.map(pack)));
    for(let i=0;i<32;i++) constants.set([scale,...(colors[i%8].slice(0,3).map(v=>(v/255)**2))],8+i*4);
    constants[140]=strength; gl.bindBuffer(gl.UNIFORM_BUFFER,ubo); gl.bufferData(gl.UNIFORM_BUFFER,constants,gl.DYNAMIC_DRAW);
    gl.vertexAttrib4f(region,x-span*.5,y-span*.5,span,0); gl.drawArrays(gl.TRIANGLES,0,6);
    const output=[]; for(let i=0;i<2;i++){gl.readBuffer(gl.COLOR_ATTACHMENT0+i);const b=new Uint8Array(4);gl.readPixels(slot%atlasCount,Math.floor(slot/atlasCount),1,1,gl.RGBA,gl.UNSIGNED_BYTE,b);output.push(...b);}
    if(gl.getError()!==gl.NO_ERROR) throw new Error('WebGL error'); return output;
  };
  let checks=0;
  const close=(a,b,label,tolerance=2)=>{if(a.some((v,i)=>Math.abs(v-b[i])>tolerance))throw new Error(label+': '+a+' vs '+b);checks++;};
  const expected=(pairs,x=.5,y=.5)=>{
    const spatial=x>=y?[1-x,x-y,0,y]:[1-y,0,y-x,x], w=new Array(8).fill(0);
    pairs.forEach(([a,b,t],i)=>{w[a]+=spatial[i]*(1-t/63);w[b]+=spatial[i]*t/63;});
    return [0,1,2].map(c=>255*w.reduce((s,t,i)=>s+t*(colors[i][c]/255)**2,0)).concat(255,128,128,...[2,3].map(c=>w.reduce((s,t,i)=>s+t*normals[i][c],0)));
  };
  const repeat=p=>[p,p,p,p];
  close(render(repeat([0,1,0])),expected(repeat([0,0,0])),'pure bottom');
  close(render(repeat([1,0,63])),expected(repeat([0,0,0])),'pure top');
  close(render(repeat([3,3,29])),expected(repeat([3,3,29])),'identical IDs');
  const pairs=[[0,1,15],[1,2,29],[0,2,44],[2,0,55]];
  close(render(pairs,{strength:0}),expected(pairs),'disabled height blend');
  close(render(pairs,{scale:64}),render(pairs),'height blending retained at coarse footprint');
  close(render(repeat([0,1,32])),expected(repeat([1,1,63])),'higher material covers lower');
  close(render(repeat([0,1,32])),render(repeat([1,0,31])),'pair reversal');
  const repeated=[[0,1,63],[0,2,20],[3,0,50],[0,1,0]];
  close(render(repeated,{strength:0}),expected(repeated),'first duplicate has zero weight');
  close(render(repeated,{x:0,y:0}),expected(repeat([1,1,0])),'zero spatial weights');
  // Both cells share the right/left edge but have unrelated outer corners.
  const left=[[0,2,20],[0,1,32],[1,2,40],[1,2,28]];
  const right=[[0,1,32],[0,2,5],[1,2,28],[0,2,60]];
  close(render(left,{x:1,y:.37}),render(right,{x:0,y:.37}),'shared cell edge');
  let seed=83;const rand=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296;};
  for(let n=0;n<100;n++){
    const q=Array.from({length:4},()=>[[0,2,7][Math.floor(rand()*3)],[0,2,7][Math.floor(rand()*3)],Math.floor(rand()*64)]);
    const x=rand(),y=rand();
    close(render(q,{strength:0,x,y}),expected(q,x,y),'random linear recovery '+n);
    close(render(q,{scale:64,x,y}),render(q,{x,y}),'random distance independence '+n);
    const swapped=q.map(([a,b,w])=>[b,a,63-w]);
    close(render(q,{x,y}),render(swapped,{x,y}),'random pair symmetry '+n);
  }

  const a=[[0,1,30],[1,2,20],[6,7,10],[0,2,50]];
  const b=a.map(v=>v.slice());b[2]=[3,4,60];
  close(render(a,{x:.75,y:.25}),render(b,{x:.75,y:.25}),'lower triangle ignores fourth texel',0);
  a[1]=[5,6,20];a[2]=[1,2,32];b[1]=[3,4,0];b[2]=[1,2,32];
  close(render(a,{x:.25,y:.75}),render(b,{x:.25,y:.75}),'upper triangle ignores fourth texel',0);
  const diagonal=[[0,1,12],[1,2,31],[0,2,45],[0,1,52]];
  close(render(diagonal,{x:.49999,y:.5}),render(diagonal,{x:.50001,y:.5}),'diagonal continuity');


  const plain=render(repeat([0,0,0]));
  constants.set([0,0,1,0],144);constants[656]=1;
  const overlay=render(repeat([0,0,0]));
  close(overlay.slice(0,3),plain.slice(0,3).map((v,i)=>v*(1-128/255)+[50,25,10][i]),'premultiplied decal color');
  close(overlay.slice(6),[plain[6]*(1-128/255)+128*128/255,plain[7]*(1-128/255)+255*128/255],'decal roughness/AO');
  close(render(repeat([0,0,0]),{x:-.1}),plain,'outside decal coverage unchanged',0);
  close(render(repeat([0,0,0]),{x:1.1}),plain,'opposite decal edge unchanged',0);
  if(overlay[4]<=plain[4]||overlay[5]>=plain[5])throw Error('Decal normal direction lost');checks++;

  // Ground-color modulation must retain the underlying material's hue.
  constants[660]=1;
  const modulated=render(repeat([0,0,0]));
  close(modulated.slice(0,3),plain.slice(0,3).map((v,i)=>v*(1-128/255+[50,25,10][i]/255)),'imprint preserves ground color');
  constants[660]=0;
  // A constant one-texel coverage map must not limit repeated material detail.
  {
  constants[661]=2;constants[662]=3; // material IDs 1 and 2, encoded +1
  const detail=render(repeat([0,0,0]));
  const weight=128/255,alpha=128/255;
  close(detail.slice(0,3),plain.slice(0,3).map((v,c)=>v*(1-alpha)+255*alpha*((colors[1][c]/255)**2*(1-weight)+(colors[2][c]/255)**2*weight)),'tiled decal color and alpha');
  close(detail.slice(6),[6,7].map((at)=>plain[at]*(1-alpha)+(normals[1][at-4]*(1-weight)+normals[2][at-4]*weight)*alpha),'tiled decal roughness and AO');
  const stripes=new Uint8Array(8*8*4);
  for(let layer=0;layer<8;layer++)for(let x=0;x<8;x++)stripes.set(layer===1||layer===2?[x%2*255,x%2*255,x%2*255,0]:colors[layer],(layer*8+x)*4);
  gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D_ARRAY,ah);
  gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_S,gl.REPEAT);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,8,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,stripes);
  constants.set([0,0,4,0],144);
  const dark=render(repeat([0,0,0]),{x:.0625,span:.001}),light=render(repeat([0,0,0]),{x:.1875,span:.001});
  close(dark.slice(0,3),plain.slice(0,3).map(v=>v*(1-alpha)),'sub-mask-texel dark detail');
  close(light.slice(0,3),plain.slice(0,3).map(v=>v*(1-alpha)+255*alpha),'sub-mask-texel light detail');
  close(render(repeat([0,0,0]),{x:1.1875,span:.001}),light,'detail repeats in meters');
  close(render(repeat([0,0,0]),{x:.09375,scale:2,span:.001}),light,'material texel density independent of decal footprint');
  constants.set([0,0,1,0],144);
  close(render(repeat([0,0,0]),{x:1.1875,span:.001}),plain,'detail respects coverage bounds');
  gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D_ARRAY,ah);
  gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(colors.flat()));
  constants[661]=0;constants[662]=0;
  }
  // A page-local list can address the last global decal without drawing the
  // intervening entries. This covers IDs beyond the previous 16-decal limit.
  constants.set([0,0,1,0],144+127*4);constants.set([10,10,1,0],144);
  indexBytes[0]=127;gl.activeTexture(gl.TEXTURE9);gl.bindTexture(gl.TEXTURE_2D,decalIndices);
  gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,32,4,gl.RGBA,gl.UNSIGNED_BYTE,indexBytes);
  close(render(repeat([0,0,0])),overlay,'page list reaches global decal 127');
  constants[656]=0;close(render(repeat([0,0,0])),plain,'empty page skips all global decals');
  constants[656]=1;indexBytes[0]=0;gl.activeTexture(gl.TEXTURE9);gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,32,4,gl.RGBA,gl.UNSIGNED_BYTE,indexBytes);
  close(render(repeat([0,0,0])),plain,'page list excludes overlapping unlisted decals');

  const link=(vs,fs,feedback)=>{
    const p=gl.createProgram();gl.attachShader(p,compile(gl.VERTEX_SHADER,vs));gl.attachShader(p,compile(gl.FRAGMENT_SHADER,fs));
    if(feedback)gl.transformFeedbackVaryings(p,['probe'],gl.INTERLEAVED_ATTRIBS);
    gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(p));return p;
  };
  for(const shader of baseEffect.shaders)for(const unlit of [0,1])for(const decal of [0,1]){
    const defines=shader.defines.map(d=>'#define '+d.name+' '+(d.name==='USE_INSTANCING'?1:d.name==='LANDSCAPE_DEBUG_UNLIT'?unlit:d.name==='LANDSCAPE_DECAL_MESH'?decal:(d.default??d.range?.[0]??0))).join('\n');
    const prefix='precision highp sampler2DArray;\n#define CC_DEVICE_SUPPORT_FLOAT_TEXTURE 0\n#define CC_PLATFORM_ANDROID_AND_WEBGL 0\n#define CC_ENABLE_WEBGL_HIGHP_STRUCT_VALUES 0\n'+defines+'\n';
    link(prefix+shader.glsl3.vert+'\n',prefix+shader.glsl3.frag+'\n');
  }
  const probeSource=decal=>[
    'precision highp float;precision highp sampler2DArray;',
    '#define USE_INSTANCING 1','#define LANDSCAPE_DECAL_MESH '+decal,
    '#define CCGetWorldMatrix(m) m = mat4(1.0)',
    'uniform vec4 heightParams,terrainParams,morphCameraPos,decalControl,lodMorph[9];',
    'struct SurfacesStandardVertexIntermediate {vec3 position;vec3 normal;vec4 tangent;vec2 texCoord;};',
    vertexChunk,
    'uniform vec2 testUV;out vec4 probe;',
    'void main(){SurfacesStandardVertexIntermediate v;v.position=vec3(testUV,0);probe=vec4(SurfacesVertexModifyLocalPos(v),v_normalMorph);gl_Position=vec4(0,0,0,1);}'
  ].join('\n');
  const probePrograms=[link(probeSource(0),'precision highp float;out vec4 c;void main(){c=vec4(1);}',true),link(probeSource(1),'precision highp float;out vec4 c;void main(){c=vec4(1);}',true)];
  const outBuffer=gl.createBuffer();gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER,outBuffer);gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER,16,gl.DYNAMIC_READ);gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER,0,outBuffer);
  const terrainTexture=gl.createTexture();gl.activeTexture(gl.TEXTURE7);gl.bindTexture(gl.TEXTURE_2D_ARRAY,terrainTexture);
  gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  const heights=new Uint8Array(17*17*4);
  for(let z=0;z<17;z++)for(let x=0;x<17;x++){const h=Math.round((.2+.1*Math.sin(x*.8)*Math.cos(z*.4))*65535),at=(z*17+x)*4;heights.set([h>>8,h&255,0,255],at);}
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,17,17,1,0,gl.RGBA,gl.UNSIGNED_BYTE,heights);
  gl.activeTexture(gl.TEXTURE8);gl.bindTexture(gl.TEXTURE_2D_ARRAY,gl.createTexture());gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MAG_FILTER,gl.LINEAR);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([128,0,0,255]));
  const probe=(uv,{decal=1,scale=0,enabled=1,camera=[-2,4,-2],range=[2,12],quadrant=[0,0,.5,0],grid=[0,0,8,8],nodeSize=16,fade=1}={})=>{
    const p=probePrograms[decal];gl.useProgram(p);
    const u=(n,...v)=>gl.uniform4f(gl.getUniformLocation(p,n),...v);
    const a=(n,...v)=>{const l=gl.getAttribLocation(p,n);if(l>=0){gl.disableVertexAttribArray(l);gl.vertexAttrib4f(l,...v);}};
    a('a_gridInst',0,0,nodeSize,0);a('a_quadrantInst',...quadrant);a('a_tileInst',0,0,16,0);a('a_normalParentInst',0,0,16,0);a('a_vtInst',0,0,16,0);a('a_decalRegion',0,0,16,0);a('a_decalGrid',...grid);a('a_decalFade',scale,fade,0,0);
    u('heightParams',1,17,0,0);u('terrainParams',10,0,0,0);u('morphCameraPos',...camera,0);u('decalControl',enabled,0,0,0);u('lodMorph[0]',...range,0,0);
    gl.uniform2f(gl.getUniformLocation(p,'testUV'),...uv);gl.uniform1i(gl.getUniformLocation(p,'heightmap'),7);gl.uniform1i(gl.getUniformLocation(p,'decalHeightMap'),8);
    gl.enable(gl.RASTERIZER_DISCARD);gl.beginTransformFeedback(gl.POINTS);gl.drawArrays(gl.POINTS,0,1);gl.endTransformFeedback();gl.disable(gl.RASTERIZER_DISCARD);
    const result=new Float32Array(4);gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER,0,result);return [...result];
  };
  const approx=(a,b,label)=>close(a,b,label,3e-5);
  // Reference geometry comes from executing the unmodified terrain variant,
  // then locating the rendered triangle on the CPU at a fixed XZ coordinate.
  // This is deliberately independent of the decal shader's 2x2 block search.
  const terrainMesh=options=>Array.from({length:17*17},(_,i)=>
    probe([i%17/16,Math.floor(i/17)/16],{...options,decal:0,quadrant:[0,0,1,0]}));
  const onTerrain=(mesh,xz)=>{
    for(let z=0;z<16;z++)for(let x=0;x<16;x++){
      const i=z*17+x;
      for(const ids of [[i,i+1,i+17],[i+18,i+17,i+1]]){
        const [a,b,c]=ids.map(i=>mesh[i]);
        const ab=[b[0]-a[0],b[2]-a[2]],ac=[c[0]-a[0],c[2]-a[2]],ap=[xz[0]-a[0],xz[1]-a[2]];
        const det=ab[0]*ac[1]-ab[1]*ac[0];if(Math.abs(det)<1e-8)continue;
        const u=(ap[0]*ac[1]-ap[1]*ac[0])/det,v=(ab[0]*ap[1]-ab[1]*ap[0])/det;
        if(Math.min(u,v,1-u-v)<-1e-6)continue;
        return [xz[0],a[1]*(1-u-v)+b[1]*u+c[1]*v,xz[1],a[3]*(1-u-v)+b[3]*u+c[3]*v];
      }
    }
    throw Error('No reference triangle at '+xz);
  };
  const mesh=terrainMesh({});
  for(let i=0;i<80;i++){
    const uv=[.02+rand()*.96,.02+rand()*.96];
    const flat=probe(uv);
    approx(flat,onTerrain(mesh,uv.map(v=>v*8)),'fixed vertex lies on actual morphed terrain');
    const raised=probe(uv,{scale:.6});
    approx([raised[0],raised[2],raised[3]],[flat[0],flat[2],flat[3]],'raising preserves VT position and normal morph');
    approx([raised[1]-flat[1]],[32768/65535*.6],'downloaded height decoding');
    approx(probe(uv,{scale:.6,enabled:0}),flat,'F12 recovers planar geometry');
  }
  for(const x of [.13,.4,.9]){
    approx(probe([1,x],{scale:.6,grid:[0,0,1,1]}),probe([0,x],{scale:.6,grid:[1,0,1,1]}),'shared fixed-grid boundary');
  }
  for(const fade of [0,.001,.1,.5,.9,1])for(const uv of [[.3,.6],[.9,.1],[.1,.2]]){
    const flat=probe(uv),raised=probe(uv,{fade,scale:.6});
    approx([raised[1]-flat[1]],[32768/65535*.6*fade],'one fade factor for the whole decal');
  }
  // All morph states, including near-degenerate triangles and complete collapse.
  const sampleUVs=[[0,0],[1,1],[.125,.125],[.24999,.25001],[.9,.01]];
  for(let i=0;i<15;i++)sampleUVs.push([rand(),rand()]);
  for(const range of [[100,200],[2,20],[2,12],[0,8],[0,.001]]){
    const options={range},mesh=terrainMesh(options);
    for(const uv of sampleUVs){
      const a=probe(uv,options);
      approx([a[0],a[2]],uv.map(v=>v*8),'XZ never follows terrain morph');
      approx(a,onTerrain(mesh,uv.map(v=>v*8)),'height at fixed point matches selected triangles');
    }
  }
  // A fine node at full morph must join the next coarser unmorphed node.
  for(const uv of sampleUVs){
    const fine=probe(uv,{range:[0,.001]}),coarse=probe(uv,{nodeSize:32,range:[100,200]});
    approx(fine.slice(0,3),coarse.slice(0,3),'fine-to-coarse LOD transition is continuous');
    approx(probe(uv,{quadrant:[.5,.5,.25,0]}),probe(uv),'VT patch ownership cannot retessellate a fixed grid');
  }
  // A high-frequency decal exposes the former bug: moving terrain vertices
  // used to sweep over peaks in this map and visibly change the stone shape.
  const detail=new Uint8Array(17*17*4);
  for(let z=0;z<17;z++)for(let x=0;x<17;x++){
    const h=Math.round((.5+.45*Math.sin(x*2)*Math.cos(z*1.7))*65535);
    detail.set([h>>8,h&255,0,255],(z*17+x)*4);
  }
  gl.activeTexture(gl.TEXTURE8);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,17,17,1,0,gl.RGBA,gl.UNSIGNED_BYTE,detail);
  for(const uv of sampleUVs){
    const reference=probe(uv,{scale:.6})[1]-probe(uv)[1];
    for(let frame=0;frame<=20;frame++){
      const options={camera:[-2+frame*.25,4,-2]};
      const flat=probe(uv,options),raised=probe(uv,{...options,scale:.6});
      approx([raised[1]-flat[1]],[reference],'camera motion preserves the sampled stone height');
      approx([raised[0],raised[2]],uv.map(v=>v*8),'camera motion preserves stone XZ');
    }
  }
  if(gl.getError()!==gl.NO_ERROR)throw Error('GPU probe error');
  result.textContent='PASS: all base/decal/unlit/shadow variants linked; '+checks+' pixel checks passed.';
} catch(e) { result.textContent='FAIL: '+e.stack; }
