// Browser-side GPU regression harness; rvt-normal-baking.test.js injects the compiled effect.
const result = document.getElementById('result');
try {
  const gl = document.createElement('canvas').getContext('webgl2');
  if (!gl) throw new Error('WebGL2 unavailable');
  gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
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
  const terrain=tex('terrainNormalMap',4,gl.TEXTURE_2D_ARRAY);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RG8,1,1,4,0,gl.RG,gl.UNSIGNED_BYTE,new Uint8Array([128,128,128,128,128,128,128,128]));
  tex('decalAlbedoMap',6,gl.TEXTURE_2D_ARRAY);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(4));
  tex('decalNormalMap',7,gl.TEXTURE_2D_ARRAY);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([128,128,255,255]));
  tex('decalIndexMap',10,gl.TEXTURE_2D);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(4));
  const sourceTable=tex('normalSourceMap',11,gl.TEXTURE_2D);
  const sourceRegions=new Float32Array(16*4);
  for(let q=0;q<16;q++)sourceRegions.set([0,0,1,Math.min(1,Math.max(0,q%4-1))+2*Math.min(1,Math.max(0,Math.floor(q/4)-1))],q*4);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA32F,16,1,0,gl.RGBA,gl.FLOAT,sourceRegions);
  const fb=gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER,fb);
  gl.activeTexture(gl.TEXTURE0+5);
  for(let i=0;i<2;i++) { const t=gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D,t); gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,2,2,0,gl.RGBA,gl.UNSIGNED_BYTE,null); gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0+i,gl.TEXTURE_2D,t,0); }
  gl.drawBuffers([gl.COLOR_ATTACHMENT0,gl.COLOR_ATTACHMENT1]);
  if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE) throw new Error('Incomplete framebuffer');
  gl.viewport(0,0,1,1);
  const pack=([a,b,w])=>a|(b<<5)|(w<<10);
  const render=(pairs,{strength=1,scale=1,x=.5,y=.5,span=1,atlasCount=1,slot=0,sourceX=0,sourceY=0,sourceSize=1,sourceLayer=0}={})=>{
    constants[0]=atlasCount;gl.viewport(0,0,atlasCount,atlasCount);
    gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtPage'),slot,0,0,0);
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
    return [0,1,2].map(c=>255*w.reduce((s,t,i)=>s+t*(colors[i][c]/255)**2,0)).concat(128,128,255,...[2,3].map(c=>w.reduce((s,t,i)=>s+t*normals[i][c],0)));
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


  // Execute composition with four distinct streamed normal sources. CPU vectors
  // below are independent geometric expectations, not a second shader copy.
  const unit=v=>{const l=Math.hypot(...v);return v.map(x=>x/l);};
  const enc=v=>v.map(x=>Math.round(x*127.5+127.5));
  const decode=b=>unit([b[0]/127.5-1,Math.sqrt(Math.max(0,1-(b[0]/127.5-1)**2-(b[1]/127.5-1)**2)),b[1]/127.5-1]);
  const terrainBytes=new Uint8Array([128,128,204,128,128,204,30,190]);
  gl.activeTexture(gl.TEXTURE4);gl.bindTexture(gl.TEXTURE_2D_ARRAY,terrain);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RG8,1,1,4,0,gl.RG,gl.UNSIGNED_BYTE,terrainBytes);
  gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D_ARRAY,splat);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.R16UI,2,2,1,0,gl.RED_INTEGER,gl.UNSIGNED_SHORT,new Uint16Array(4));
  const readPixel=(x,y)=>{const out=[];for(let i=0;i<2;i++){gl.readBuffer(gl.COLOR_ATTACHMENT0+i);const b=new Uint8Array(4);gl.readPixels(x,y,1,1,gl.RGBA,gl.UNSIGNED_BYTE,b);out.push(...b);}return out;};
  const compose=(baked,detail)=>{
    gl.useProgram(p);gl.bindFramebuffer(gl.FRAMEBUFFER,fb);gl.viewport(0,0,2,2);
    constants.set([2,2,0,1],0);constants[5]=baked?1:0;
    gl.bindBuffer(gl.UNIFORM_BUFFER,ubo);gl.bufferData(gl.UNIFORM_BUFFER,constants,gl.DYNAMIC_DRAW);
    gl.vertexAttrib4f(region,0,0,1,0);gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtSource'),0,0,1,0);
    gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtPage'),0,constants?.[656]||0,0,0);
    gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D_ARRAY,nra);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(Array.from({length:8},()=>[...detail,80,210]).flat()));
    gl.drawArrays(gl.TRIANGLES,0,6);
    return Array.from({length:4},(_,q)=>readPixel(q%2,q>>1));
  };
  for(const detail of [[128,128],[204,128],[128,30],[250,128],[40,200]]){
    const off=compose(false,detail),on=compose(true,detail);
    const dx=detail[0]/127.5-1,dy=detail[1]/127.5-1;
    const d=unit([dx,dy,Math.sqrt(Math.max(0,1-dx*dx-dy*dy))]);
    for(let q=0;q<4;q++){
      close([off[q][4],off[q][3],off[q][5]],enc(d),'OFF tangent normal');
      const ng=decode(terrainBytes.slice(q*2,q*2+2)),t=unit([ng[1],-ng[0],0]);
      const bt=[ng[1]*t[2]-ng[2]*t[1],ng[2]*t[0]-ng[0]*t[2],ng[0]*t[1]-ng[1]*t[0]];
      const expected=unit(ng.map((n,i)=>t[i]*d[0]+bt[i]*d[1]+n*d[2]));
      close([on[q][4],on[q][3],on[q][5]],enc(expected),'ON quadrant '+q);
      close(on[q].slice(0,3),off[q].slice(0,3),'unchanged color',0);
      close(on[q].slice(6),off[q].slice(6),'unchanged roughness and AO',0);
    }
  }
  const below=compose(true,[250,128])[1];
  if(below[3]>=128)throw new Error('Negative vertical normal was lost');checks++;

  // An opaque decal must replace the material normal BEFORE the terrain frame
  // is applied. This also covers the normal consumed by raised decal meshes.
  const decalReference=compose(true,[204,128]);
  constants.set([0,0,1,0],144);constants[656]=1;
  gl.activeTexture(gl.TEXTURE6);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([50,25,10,255]));
  gl.activeTexture(gl.TEXTURE7);gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([204,128,80,210]));
  const decalBaked=compose(true,[30,220]);
  for(let q=0;q<4;q++){
    close(decalBaked[q].slice(3),decalReference[q].slice(3),'decal and geometry normal composition order');
    close(decalBaked[q].slice(0,3),[50,25,10],'decal color survives signed normal packing');
  }
  constants[656]=0;

  // Render a page INCLUDING its gutters. Sixteen distinct source regions
  // expose accidental edge clamping or incorrect neighbor/row selection.
  {
    const size=12,bytes=new Uint8Array(16*2);
    for(let q=0;q<16;q++){
      bytes.set([75+(q%4)*30,80+Math.floor(q/4)*28],q*2);
      sourceRegions.set([(q%4-1)*.5,(Math.floor(q/4)-1)*.5,.5,q],q*4);
    }
    gl.useProgram(p);gl.activeTexture(gl.TEXTURE4);gl.bindTexture(gl.TEXTURE_2D_ARRAY,terrain);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RG8,1,1,16,0,gl.RG,gl.UNSIGNED_BYTE,bytes);
    gl.activeTexture(gl.TEXTURE11);gl.bindTexture(gl.TEXTURE_2D,sourceTable);
    gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,16,1,gl.RGBA,gl.FLOAT,sourceRegions);
    gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D_ARRAY,nra);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RGBA8,1,1,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(Array.from({length:8},()=>[128,128,80,210]).flat()));
    gl.activeTexture(gl.TEXTURE5);
    for(let i=0;i<2;i++){
      const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
      gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,size,size,0,gl.RGBA,gl.UNSIGNED_BYTE,null);
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0+i,gl.TEXTURE_2D,t,0);
    }
    gl.vertexAttrib4f(gl.getAttribLocation(p,'a_vtPage'),0,0,0,0);
    constants.set([size,size,2,1],0);constants[5]=1;
    gl.bindBuffer(gl.UNIFORM_BUFFER,ubo);gl.bufferData(gl.UNIFORM_BUFFER,constants,gl.DYNAMIC_DRAW);
    gl.viewport(0,0,size,size);gl.drawArrays(gl.TRIANGLES,0,6);
    for(let y=0;y<size;y++)for(let x=0;x<size;x++){
      const cell=v=>Math.max(0,Math.min(3,Math.floor(((v+.5)-2)/8*2)+1));
      const q=cell(x)+4*cell(y),pixel=readPixel(x,y);
      close([pixel[4],pixel[3],pixel[5]],enc(decode(bytes.slice(q*2,q*2+2))),'normal gutter neighbor '+x+','+y,3);
    }
    // Restore the original sources used below by the Base Pass baseline.
    gl.activeTexture(gl.TEXTURE4);gl.bindTexture(gl.TEXTURE_2D_ARRAY,terrain);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY,0,gl.RG8,1,1,4,0,gl.RG,gl.UNSIGNED_BYTE,terrainBytes);
  }

  // Run both production downsample passes, including a negative-Y normal.
  const mip=programs[1];gl.useProgram(mip);
  const mipPosition=gl.getAttribLocation(mip,'a_position');gl.enableVertexAttribArray(mipPosition);gl.vertexAttribPointer(mipPosition,2,gl.FLOAT,false,0,0);
  gl.vertexAttrib4f(gl.getAttribLocation(mip,'a_vtPage'),0,0,0,0);
  gl.uniformBlockBinding(mip,gl.getUniformBlockIndex(mip,'MipConstants'),0);
  const normals4=[[.4,-.8,.4],[-.2,-.9,.3],[.5,-.7,-.4],[-.3,-.8,-.3]].map(unit);
  let sourceColors=new Uint8Array(4*4*4),sourceNormals=new Uint8Array(4*4*4);
  for(let y=0;y<4;y++)for(let x=0;x<4;x++){
    const n=enc(normals4[(x%2)+(y%2)*2]),at=(y*4+x)*4;
    sourceColors.set([32,64,96,n[1]],at);sourceNormals.set([n[0],n[2],80,210],at);
  }
  const expectedMean=enc(unit(normals4.reduce((a,n)=>a.map((x,i)=>x+n[i]),[0,0,0])));
  for(const sourceSize of [4,2]){
    for(const [name,textureUnit,data]of[['sourceAlbedo',6,sourceColors],['sourceNormal',7,sourceNormals]]){
      gl.activeTexture(gl.TEXTURE0+textureUnit);const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);
      gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
      gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,sourceSize,sourceSize,0,gl.RGBA,gl.UNSIGNED_BYTE,data);
      gl.uniform1i(gl.getUniformLocation(mip,name),textureUnit);
    }
    const size=sourceSize/2;gl.viewport(0,0,size,size);
    gl.bindBuffer(gl.UNIFORM_BUFFER,ubo);gl.bufferData(gl.UNIFORM_BUFFER,new Float32Array([sourceSize,sourceSize,0,1]),gl.DYNAMIC_DRAW);
    gl.drawArrays(gl.TRIANGLES,0,6);
    sourceColors=new Uint8Array(size*size*4);sourceNormals=new Uint8Array(size*size*4);
    for(let y=0;y<size;y++)for(let x=0;x<size;x++){
      const pixel=readPixel(x,y),at=(y*size+x)*4;
      close([pixel[4],pixel[3],pixel[5]],expectedMean,'filtered signed normal, mip '+(sourceSize===4?1:2));
      close([...pixel.slice(0,3),...pixel.slice(6)],[32,64,96,80,210],'mip material channels',0);
      sourceColors.set(pixel.slice(0,4),at);sourceNormals.set(pixel.slice(4),at);
    }
  }
  if(gl.getError()!==gl.NO_ERROR)throw new Error('WebGL normal/mip error');


  // Compile the full Base Pass + shadow variants, then run its production
  // surface function against known normals without unrelated engine lighting.
  const link=(vs,fs)=>{
    const program=gl.createProgram();gl.attachShader(program,compile(gl.VERTEX_SHADER,vs));gl.attachShader(program,compile(gl.FRAGMENT_SHADER,fs));gl.linkProgram(program);
    if(!gl.getProgramParameter(program,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(program));return program;
  };
  for(const shader of compiledBase.shaders)for(const unlit of [0,1]){
    const defines=shader.defines.map(d=>'#define '+d.name+' '+(d.name==='USE_INSTANCING'?1:d.name==='LANDSCAPE_DEBUG_UNLIT'?unlit:(d.default??d.range?.[0]??0))).join('\n');
    const prefix='precision highp sampler2DArray;\n#define CC_DEVICE_SUPPORT_FLOAT_TEXTURE 0\n#define CC_PLATFORM_ANDROID_AND_WEBGL 0\n#define CC_ENABLE_WEBGL_HIGHP_STRUCT_VALUES 0\n'+defines+'\n';
    link(prefix+shader.glsl3.vert+'\n',prefix+shader.glsl3.frag+'\n');
  }
  const vs='precision highp float;in vec2 a_position;void main(){gl_Position=vec4(a_position*2.0-1.0,0.0,1.0);}';
  const fs=unlit=>[
    'precision highp float;precision highp int;precision highp sampler2DArray;',
    '#define LANDSCAPE_DEBUG_UNLIT '+unlit,'#define LANDSCAPE_DECAL_MESH 0','#define LANDSCAPE_MAX_LOD_LEVELS 9',
    'uniform sampler2D vtAlbedo,vtNormalRoughnessAO;uniform sampler2DArray terrainNormalMap;',
    'uniform vec4 mainColor,terrainParams,morphCameraPos,vtLayout,rvtNormalParams,v_vtParams,lodMorph[9];',
    'uniform float v_normalMorph;const float v_lod=0.0;',
    'const vec4 v_normalUV=vec4(0.5);const vec2 v_normalLayers=vec2(0.0,1.0);',
    'const vec2 FSInput_texcoord=vec2(0.5,-0.5);const vec3 FSInput_worldPos=vec3(0);',
    'const vec3 FSInput_worldNormal=vec3(0,1,0),FSInput_worldTangent=vec3(1,0,0);const float FSInput_mirrorNormal=1.0;',
    'struct SurfacesMaterialData {vec4 baseColor;vec3 worldNormal;float roughness;float ao;float metallic;float specularIntensity;vec3 emissive;};',
    'vec3 CalculateNormalFromTangentSpace(vec3 v,float strength,vec3 n,vec3 t,float mirror){return vec3(v.x,v.z,-v.y);}',
    shaderFunctions,
    'layout(location=0)out vec4 color;layout(location=1)out vec4 normal;',
    'void main(){SurfacesMaterialData d;d.worldNormal=vec3(0,1,0);d.ao=1.0;SurfacesFragmentModifySharedData(d);color=d.baseColor;normal=vec4(d.worldNormal*0.5+0.5,d.ao);}'
  ].join('\n');
  const basePrograms=[link(vs,fs(0)),link(vs,fs(1))];
  const baseTextures=[];
  for(let i=0;i<2;i++){
    gl.activeTexture(gl.TEXTURE0+8+i);const t=gl.createTexture();baseTextures.push(t);gl.bindTexture(gl.TEXTURE_2D,t);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
  }
  const baseRender=(baked,morph,n,unlit=0)=>{
    const program=basePrograms[unlit];gl.useProgram(program);
    const pos=gl.getAttribLocation(program,'a_position');gl.enableVertexAttribArray(pos);gl.vertexAttribPointer(pos,2,gl.FLOAT,false,0,0);
    const u=(name,...values)=>gl.uniform4f(gl.getUniformLocation(program,name),...values);
    u('mainColor',1,1,1,1);u('vtLayout',1,1,0,0);u('v_vtParams',0,0,1,0);u('rvtNormalParams',baked?1:0,0,0,0);
    gl.uniform1f(gl.getUniformLocation(program,'v_normalMorph'),morph);
    gl.uniform1i(gl.getUniformLocation(program,'terrainNormalMap'),4);
    gl.uniform1i(gl.getUniformLocation(program,'vtAlbedo'),8);gl.uniform1i(gl.getUniformLocation(program,'vtNormalRoughnessAO'),9);
    const packed=enc(n);
    for(const [i,data]of[[0,[32,64,96,packed[1]]],[1,[packed[0],packed[2],80,210]]]){
      gl.activeTexture(gl.TEXTURE0+8+i);gl.bindTexture(gl.TEXTURE_2D,baseTextures[i]);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(data));
    }
    gl.viewport(0,0,1,1);gl.drawArrays(gl.TRIANGLES,0,6);return readPixel(0,0);
  };
  for(const n of [[.4,.8,.2],[.5,-.7,.3],[-.3,.1,-.8]].map(unit)){
    const reference=baseRender(true,0,n);
    close(reference.slice(4,7),enc(n),'Base Pass local-to-world normal');
    for(const morph of [0,.3,.8,1]){
      close(baseRender(true,morph,n),reference,'baked normal independent of geometry morph',0);
      close(baseRender(true,morph,n).slice(0,4),[32,64,96,255],'normal Y never changes opacity',0);
      close(baseRender(true,morph,n,1).slice(0,4),[32,64,96,255],'F7 unchanged',0);
    }
  }
  for(const morph of [0,.3,.8,1]){
    const xz=[0,1].map(i=>terrainBytes[i]*(1-morph)+terrainBytes[2+i]*morph);
    close(baseRender(false,morph,[0,0,1]).slice(4,7),enc(decode(xz)),'OFF retains geometry normal morph');
  }
  if(gl.getError()!==gl.NO_ERROR)throw new Error('WebGL Base Pass error');
  result.textContent='PASS: compose, mip, Base Pass and shadow GLSL ES 3 variants compiled and linked; '+checks+' pixel checks passed.';
} catch(e) { result.textContent='FAIL: '+e.stack; }
