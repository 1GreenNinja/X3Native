// HoloPanel — the reusable holo-glass platform. See app/holo_panel.h.
//
// Clean-room: Scene/Entity + IRenderDevice + mesh_prims + stb_truetype only.
// The screen pane is a glossy black DIELECTRIC (metallic-roughness map) whose
// EMISSIVE is driven by a baked RGBA texture — so glowing text/line-art reads on
// dark glass, no scene-copy dependency (the flagship terminal's proven recipe).
#include "holo_panel.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/font_robotomono.h"
#include <stb_truetype.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {
constexpr float kPi = 3.14159265f;

// ---- Compact RGBA baker (a self-contained mini of the terminal's canvas) -------
struct Canvas {
    uint32_t n;
    std::vector<float> r, g, b;
    explicit Canvas(uint32_t nn) : n(nn), r((size_t)nn*nn,0), g((size_t)nn*nn,0), b((size_t)nn*nn,0) {}
    void add(int x, int y, float rr, float gg, float bb, float a) {
        if (x < 0 || y < 0 || x >= (int)n || y >= (int)n) return;
        const size_t i = (size_t)y*n + x; r[i]+=rr*a; g[i]+=gg*a; b[i]+=bb*a;
    }
    void fillBase(float rr, float gg, float bb) {
        std::fill(r.begin(), r.end(), rr); std::fill(g.begin(), g.end(), gg); std::fill(b.begin(), b.end(), bb);
    }
};
void line(Canvas& c, float x0, float y0, float x1, float y1, float r, float g, float b, float a, float thick) {
    const float dx=x1-x0, dy=y1-y0, len=std::sqrt(dx*dx+dy*dy);
    if (len < 0.001f) { c.add((int)x0,(int)y0,r,g,b,a); return; }
    const float nx=dx/len, ny=dy/len, px=-ny, py=nx; const int steps=(int)(len+1.0f), half=(int)std::ceil(thick*0.5f);
    for (int s=0;s<=steps;++s){ const float cx=x0+nx*s, cy=y0+ny*s;
        for (int t=-half;t<=half;++t){ const float cov=1.0f-std::max(0.0f,(std::fabs((float)t)-thick*0.5f+0.5f)); if (cov<=0) continue;
            c.add((int)std::lround(cx+px*t),(int)std::lround(cy+py*t), r,g,b, a*std::min(1.0f,cov)); } }
}
void rectFrame(Canvas& c, float x0,float y0,float x1,float y1,float r,float g,float b,float a,float th){
    line(c,x0,y0,x1,y0,r,g,b,a,th); line(c,x1,y0,x1,y1,r,g,b,a,th); line(c,x1,y1,x0,y1,r,g,b,a,th); line(c,x0,y1,x0,y0,r,g,b,a,th);
}

// stb_truetype font (Roboto Mono, embedded), lazily initialised.
struct Font { stbtt_fontinfo info{}; bool ok=false;
    Font(){ const unsigned char* t=x3::rhi::kRobotoMonoTTF; int o=stbtt_GetFontOffsetForIndex(t,0); if(o>=0&&stbtt_InitFont(&info,t,o)) ok=true; } };
const Font& font(){ static Font f; return f; }
float textW(const std::string& s, float px){ const Font& f=font(); if(!f.ok) return px*0.6f*s.size();
    const float sc=stbtt_ScaleForPixelHeight(&f.info,px); float w=0; for(char ch:s){ int a=0,l=0; stbtt_GetCodepointHMetrics(&f.info,(unsigned char)ch,&a,&l); w+=a*sc; } return w; }
void drawText(Canvas& c, const std::string& s, float penX, float topY, float px, float r, float g, float b, float a){
    const Font& f=font(); if(!f.ok) return; const float sc=stbtt_ScaleForPixelHeight(&f.info,px);
    int asc=0,desc=0,lg=0; stbtt_GetFontVMetrics(&f.info,&asc,&desc,&lg); const float base=topY+asc*sc; float pen=penX;
    for(char chc:s){ const int ch=(unsigned char)chc; int adv=0,lsb=0; stbtt_GetCodepointHMetrics(&f.info,ch,&adv,&lsb);
        if(ch!=' '){ int gw=0,gh=0,gx=0,gy=0; unsigned char* bmp=stbtt_GetCodepointBitmap(&f.info,sc,sc,ch,&gw,&gh,&gx,&gy);
            if(bmp){ const float x0=pen+lsb*sc; for(int yy=0;yy<gh;++yy)for(int xx=0;xx<gw;++xx){ const float cov=bmp[yy*gw+xx]/255.0f; if(cov<=0.003f)continue;
                c.add((int)std::lround(x0+gx+xx),(int)std::lround(base+gy+yy), r,g,b, a*cov);} stbtt_FreeBitmap(bmp,nullptr);} }
        pen+=adv*sc; }
}
// Finish: apply rounded-corner + edge fade + pack to RGBA8 (matches the terminal silhouette).
std::vector<uint8_t> finish(Canvas& c){
    const uint32_t n=c.n; const float fn=(float)n; const float rc=0.30f; std::vector<uint8_t> px((size_t)n*n*4);
    auto to8=[](float v){ int i=(int)(v*255.0f+0.5f); return (uint8_t)(i<0?0:i>255?255:i); };
    for(uint32_t y=0;y<n;++y)for(uint32_t x=0;x<n;++x){ const float u=(x+0.5f)/fn, v=(y+0.5f)/fn;
        const float ax=std::fabs(u-0.5f)*2.0f, ay=std::fabs(v-0.5f)*2.0f; float fade=1.0f; const float in=1.0f-rc;
        if(ax>in&&ay>in){ const float dx=(ax-in)/rc, dy=(ay-in)/rc, cd=std::sqrt(dx*dx+dy*dy); fade=std::max(0.0f,1.0f-cd);}
        const float edge=1.0f-0.6f*std::max(0.0f,std::max(ax,ay)-0.92f)/0.08f; fade*=std::max(0.0f,edge);
        const size_t i=(size_t)y*n+x; uint8_t* p=&px[i*4]; p[0]=to8(c.r[i]*fade); p[1]=to8(c.g[i]*fade); p[2]=to8(c.b[i]*fade); p[3]=255; }
    return px;
}
void darkBase(Canvas& c){ c.fillBase(0.012f,0.020f,0.040f); }   // near-black display field

// ---- Local mesh helpers (rounded panel + Y cylinder), self-contained. ----
x3::prims::PrimMesh roundedPanel(float hw, float hh, float corner){
    x3::prims::PrimMesh m; const float r=std::min(corner,std::min(hw,hh)*0.9f); std::vector<float> ring;
    auto pt=[&](float x,float y){ ring.push_back(x); ring.push_back(y); }; const int seg=4;
    struct C{float cx,cy,a0;} cs[4]={{hw-r,hh-r,0.0f},{-hw+r,hh-r,kPi*0.5f},{-hw+r,-hh+r,kPi},{hw-r,-hh+r,kPi*1.5f}};
    for(int k=0;k<4;++k)for(int s=0;s<=seg;++s){ const float a=cs[k].a0+(kPi*0.5f)*((float)s/seg); pt(cs[k].cx+std::cos(a)*r,cs[k].cy+std::sin(a)*r);}
    const uint32_t rn=(uint32_t)(ring.size()/2);
    auto push=[&](float x,float y,float z,float nz){ const float uu=(x+hw)/(2*hw), vv=1.0f-(y+hh)/(2*hh); m.verts.push_back({{x,y,z},{0,0,nz},{uu,vv}}); };
    push(0,0,0,1); for(uint32_t i=0;i<rn;++i)push(ring[i*2],ring[i*2+1],0,1);
    for(uint32_t i=0;i<rn;++i){ m.index.push_back(0); m.index.push_back(1+i); m.index.push_back(1+((i+1)%rn)); }
    const uint32_t base=(uint32_t)m.verts.size();
    auto pushB=[&](float x,float y){ const float uu=1.0f-(x+hw)/(2*hw), vv=1.0f-(y+hh)/(2*hh); m.verts.push_back({{x,y,-0.001f},{0,0,-1},{uu,vv}}); };
    pushB(0,0); for(uint32_t i=0;i<rn;++i)pushB(ring[i*2],ring[i*2+1]);
    for(uint32_t i=0;i<rn;++i){ m.index.push_back(base); m.index.push_back(base+1+((i+1)%rn)); m.index.push_back(base+1+i); }
    return m;
}
x3::prims::PrimMesh cylinderY(float r, float halfH, uint32_t seg=20){
    x3::prims::PrimMesh m; seg=std::max(6u,seg); const float tp=6.2831853f;
    for(uint32_t j=0;j<=seg;++j){ const float a=tp*((float)j/seg), ca=std::cos(a), sa=std::sin(a);
        m.verts.push_back({{ca*r,-halfH,sa*r},{ca,0,sa},{(float)j/seg,0}}); m.verts.push_back({{ca*r,halfH,sa*r},{ca,0,sa},{(float)j/seg,1}});}
    for(uint32_t j=0;j<seg;++j){ const uint32_t b0=j*2,t0=j*2+1,b1=(j+1)*2,t1=(j+1)*2+1; m.index.insert(m.index.end(),{b0,b1,t0,t0,b1,t1}); }
    auto cap=[&](float y,float ny){ const uint32_t c=(uint32_t)m.verts.size(); m.verts.push_back({{0,y,0},{0,ny,0},{0.5f,0.5f}}); const uint32_t st=(uint32_t)m.verts.size();
        for(uint32_t j=0;j<=seg;++j){ const float a=tp*((float)j/seg); m.verts.push_back({{std::cos(a)*r,y,std::sin(a)*r},{0,ny,0},{0,0}});}
        for(uint32_t j=0;j<seg;++j){ if(ny>0)m.index.insert(m.index.end(),{c,st+j,st+j+1}); else m.index.insert(m.index.end(),{c,st+j+1,st+j}); } };
    cap(halfH,1.0f); cap(-halfH,-1.0f); return m;
}
} // namespace

// ===========================================================================
// Content bakers for the shipped variants.
// ===========================================================================
std::vector<uint8_t> bakeFloorSelect(uint32_t n, const std::vector<std::string>& floors, int sel){
    Canvas c(n); darkBase(c); const float fn=(float)n; auto P=[&](float f){return f*fn;};
    const float th=std::max(1.4f,fn/512.0f);
    rectFrame(c,P(0.08f),P(0.08f),P(0.92f),P(0.92f), 0.30f,0.66f,1.70f,0.6f,th*1.4f);       // blue border
    drawText(c,"SELECT FLOOR",P(0.14f),P(0.11f),P(0.055f), 0.55f,0.85f,1.75f,1.0f);          // header (blue)
    line(c,P(0.12f),P(0.20f),P(0.88f),P(0.20f), 0.30f,0.66f,1.70f,0.7f,th);
    // up/down chevrons
    line(c,P(0.47f),P(0.155f),P(0.50f),P(0.125f),0.6f,0.9f,1.75f,0.9f,th*1.3f); line(c,P(0.53f),P(0.155f),P(0.50f),P(0.125f),0.6f,0.9f,1.75f,0.9f,th*1.3f);
    float ty=P(0.28f); const float rowH=P(0.62f)/std::max<size_t>(1,floors.size());
    for(size_t i=0;i<floors.size();++i){ const bool onSel=((int)i==sel);
        float r=0.30f,g=0.66f,b=1.70f; if(onSel){ r=0.24f; g=1.55f; b=0.52f; }                // selected => GREEN
        if(onSel){ rectFrame(c,P(0.12f),ty-P(0.005f),P(0.88f),ty+rowH*0.72f, 0.24f,1.55f,0.52f,0.7f,th); line(c,P(0.135f),ty+rowH*0.33f,P(0.16f),ty+rowH*0.33f,0.24f,1.55f,0.52f,1.0f,th*1.4f); }
        drawText(c,floors[i],P(0.20f),ty,P(0.050f), r,g,b, onSel?1.0f:0.85f); ty+=rowH; }
    line(c,P(0.47f),P(0.845f),P(0.50f),P(0.875f),0.6f,0.9f,1.75f,0.9f,th*1.3f); line(c,P(0.53f),P(0.845f),P(0.50f),P(0.875f),0.6f,0.9f,1.75f,0.9f,th*1.3f);
    return finish(c);
}

std::vector<uint8_t> bakeKeypad(uint32_t n, const std::string& entered){
    Canvas c(n); darkBase(c); const float fn=(float)n; auto P=[&](float f){return f*fn;};
    const float th=std::max(1.4f,fn/512.0f)*1.7f;   // bolder strokes (reads small)
    drawText(c,"ENTER CODE",P(0.14f),P(0.075f),P(0.062f), 0.55f,0.85f,1.75f,1.0f);
    // entered-code line (amber) — big + prominent
    const std::string shown = entered.empty()? std::string("____") : entered;
    drawText(c,"> "+shown+"_",P(0.16f),P(0.175f),P(0.075f), 1.60f,0.88f,0.22f,1.0f);
    const char* keys[12]={"1","2","3","4","5","6","7","8","9","*","0","#"};
    const float gx0=P(0.14f), gy0=P(0.30f), cw=P(0.72f)/3.0f, chh=P(0.62f)/4.0f;
    for(int i=0;i<12;++i){ const int col=i%3, row=i/3; const float x0=gx0+col*cw, y0=gy0+row*chh;
        rectFrame(c,x0,y0,x0+cw*0.86f,y0+chh*0.82f, 0.32f,0.68f,1.72f,0.85f,th);
        const float kpx=P(0.085f); const float kw=textW(keys[i],kpx);
        drawText(c,keys[i],x0+(cw*0.86f-kw)*0.5f,y0+chh*0.16f,kpx, 0.62f,0.92f,1.78f,1.0f); }
    return finish(c);
}

std::vector<uint8_t> bakePlacard(uint32_t n, const std::vector<std::string>& lines){
    Canvas c(n); c.fillBase(0.030f,0.018f,0.006f);            // warm near-black field
    const float fn=(float)n; auto P=[&](float f){return f*fn;}; const float th=std::max(1.4f,fn/512.0f);
    rectFrame(c,P(0.08f),P(0.08f),P(0.92f),P(0.92f), 1.55f,0.80f,0.22f,0.7f,th*1.4f);         // amber border
    line(c,P(0.10f),P(0.24f),P(0.90f),P(0.24f), 1.55f,0.80f,0.22f,0.6f,th);
    if(!lines.empty()){ const float tw=textW(lines[0],P(0.070f)); float tpx=P(0.070f); if(tw>P(0.80f))tpx*=P(0.80f)/tw;
        drawText(c,lines[0],P(0.12f),P(0.11f),tpx, 1.65f,0.92f,0.35f,1.0f); }                  // title (bright amber)
    float ty=P(0.32f); for(size_t i=1;i<lines.size();++i){ float bpx=P(0.052f); const float w=textW(lines[i],bpx); if(w>P(0.78f))bpx*=P(0.78f)/w;
        drawText(c,lines[i],P(0.12f),ty,bpx, 1.45f,0.72f,0.18f,0.92f); ty+=P(0.085f); }        // body (amber)
    return finish(c);
}

// ===========================================================================
// HoloPanel fixture.
// ===========================================================================
void HoloPanel::build(Scene& scene, x3::rhi::IRenderDevice& device, const HoloPanelParams& p) {
    m_scene = &scene; m_device = &device; m_pos = p.pos; m_texN = p.texN; m_shimmer = p.shimmerIntensity;
    const float cs=std::cos(p.yaw), sn=std::sin(p.yaw);
    const float hw=p.width*0.5f, hh=p.height*0.5f;
    const float corner = (p.cornerRadius>0.0f)? p.cornerRadius : std::min(hw,hh)*0.30f;

    // Chrome + gloss metallic-roughness maps (G=roughness, B=metallic).
    auto chromeMR = x3::prims::makeSolidRGBA(4, 0, 33, 255);
    x3::rhi::TextureHandle chromeTex = device.createTexture(chromeMR.data(),4,4,false);
    const uint8_t rgh = (uint8_t)std::clamp((int)(p.paneRoughness*255.0f+0.5f),0,255);
    auto glossMR = x3::prims::makeSolidRGBA(4, 0, rgh, 0);
    x3::rhi::TextureHandle glossTex = device.createTexture(glossMR.data(),4,4,false);

    // Metal-part helper (local offset, yaw-rotated).
    auto addMetal=[&](const x3::prims::PrimMesh& g, float ox, float oy, float oz, const float col[3]){
        Entity e; e.mesh=device.createMesh(g.verts.data(),(uint32_t)g.verts.size(),g.index.data(),(uint32_t)g.index.size());
        e.mrTex=chromeTex; e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=1.0f; e.tag=(uint32_t)Tag::Prop;
        const float wx=cs*ox+sn*oz, wz=-sn*ox+cs*oz; e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=p.pos.x+wx; e.transform[13]=p.pos.y+oy; e.transform[14]=p.pos.z+wz; return scene.add(e); };

    // ---- (1) The glossy black-glass SCREEN PANE (emissive-textured dielectric). ----
    if (!p.contentBake) { x3::logError("[holopanel] no contentBake supplied"); return; }
    std::vector<uint8_t> rgba = p.contentBake(m_texN);
    m_screenTex = device.createTexture(rgba.data(), m_texN, m_texN, /*srgb*/true);
    {
        x3::prims::PrimMesh geo = roundedPanel(hw, hh, corner);
        Entity e; e.mesh=device.createMesh(geo.verts.data(),(uint32_t)geo.verts.size(),geo.index.data(),(uint32_t)geo.index.size());
        e.tex=m_screenTex; e.emissiveTex=m_screenTex; e.mrTex=glossTex;
        e.baseColor[0]=p.paneDarkness; e.baseColor[1]=p.paneDarkness*1.1f; e.baseColor[2]=p.paneDarkness*1.4f; e.baseColor[3]=1.0f;
        m_emBase[0]=p.emissiveTint[0]; m_emBase[1]=p.emissiveTint[1]; m_emBase[2]=p.emissiveTint[2]; m_emBase[3]=p.emissiveStrength;
        for(int k=0;k<4;++k) e.emissive[k]=m_emBase[k];
        e.tag=(uint32_t)Tag::Prop; e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=p.pos.x; e.transform[13]=p.pos.y; e.transform[14]=p.pos.z; m_pane=scene.add(e);
    }

    // ---- (2) FRAME around the screen edge. ----
    if (p.frame == HoloFrame::Pipe) {
        x3::prims::PrimMesh frame = x3::prims::makeRoundedRectTube(hw+0.030f, hh+0.030f, corner+0.02f, 0.028f, 8, 18);
        m_decor.push_back(addMetal(frame, 0,0,0, p.frameColor));
    } else if (p.frame == HoloFrame::Bezel) {
        // A slim DARK matte bezel: a thin round-tube ring, near-black albedo (chrome MR
        // gives it a faint edge sheen but the dark albedo keeps it matte).
        const float dark[3]={0.06f,0.065f,0.08f};
        x3::prims::PrimMesh bez = x3::prims::makeRoundedRectTube(hw+0.018f, hh+0.018f, corner+0.012f, 0.016f, 6, 12);
        m_decor.push_back(addMetal(bez, 0,0,0, dark));
    }

    // ---- (3) MOUNT. ----
    const float chrome[3]={p.frameColor[0],p.frameColor[1],p.frameColor[2]};
    const float collarCol[3]={p.frameColor[0]*0.9f,p.frameColor[1]*0.9f,p.frameColor[2]*0.9f};
    if (p.mount == HoloMount::CeilingPipe) {
        const float ceil=(p.ceilingY>0.0f)? p.ceilingY : p.pos.y+1.7f;
        const float top=p.pos.y+hh+0.030f; const float H=ceil-top;
        if (H>0.05f){ const float sr=0.026f; const float mid=(ceil+top)*0.5f-p.pos.y;
            m_decor.push_back(addMetal(cylinderY(sr,H*0.5f,20), 0,mid,0, chrome));
            m_decor.push_back(addMetal(cylinderY(sr+0.016f,0.028f,20), 0,(top-p.pos.y)+0.028f,0, collarCol));
            m_decor.push_back(addMetal(cylinderY(sr+0.026f,0.022f,20), 0,(ceil-p.pos.y)-0.022f,0, collarCol)); }
    } else if (p.mount == HoloMount::WallFlush) {
        // A slim back-box seating the panel against the wall — BEHIND the pane
        // (local -Z, away from the +Z-facing viewer), else it occludes the screen.
        x3::prims::PrimMesh box = x3::prims::makeBox(hw*0.85f, hh*0.85f, 0.03f, 0,0,0, 1.0f);
        const float dark[3]={0.10f,0.11f,0.13f};
        m_decor.push_back(addMetal(box, 0, 0, -0.05f, dark));   // -local Z = behind the front-facing pane
    } else { // FreeStand
        const float floorY=(p.floorY>0.0f)? p.floorY : p.pos.y-hh-0.6f;
        const float bottom=p.pos.y-hh-0.03f; const float H=bottom-floorY;
        if (H>0.05f){ const float sr=0.030f; const float mid=(bottom+floorY)*0.5f-p.pos.y;
            m_decor.push_back(addMetal(cylinderY(sr,H*0.5f,20), 0,mid,0, chrome));
            m_decor.push_back(addMetal(cylinderY(sr+0.020f,0.026f,20), 0,(bottom-p.pos.y)-0.026f,0, collarCol));
            m_decor.push_back(addMetal(cylinderY(0.14f,0.020f,28), 0,(floorY-p.pos.y)+0.020f,0, collarCol)); }  // base disc
    }

    // ---- (4) Glow-light suggestion (in FRONT along the panel normal). ----
    if (p.glowLight) {
        const float nx=sn, nz=cs;   // panel normal (yaw): (sin,0,cos)
        m_hasGlow=true; m_glowPos[0]=p.pos.x+nx*0.55f; m_glowPos[1]=p.pos.y; m_glowPos[2]=p.pos.z+nz*0.55f;
        m_glowColor[0]=p.glowColor[0]; m_glowColor[1]=p.glowColor[1]; m_glowColor[2]=p.glowColor[2]; m_glowRange=p.glowRange;
    }
    x3::logInfo("[holopanel] built (frame=" + std::to_string((int)p.frame) + " mount=" + std::to_string((int)p.mount) + ")");
}

void HoloPanel::setContent(const std::vector<uint8_t>& rgba) {
    if (!m_device || m_pane==kNoLink) return;
    x3::rhi::TextureHandle fresh = m_device->createTexture(rgba.data(), m_texN, m_texN, true);
    if (!fresh.valid()) return; x3::rhi::TextureHandle old=m_screenTex; m_screenTex=fresh;
    if (m_scene && m_pane<m_scene->size()){ Entity& e=m_scene->get(m_pane); e.tex=m_screenTex; e.emissiveTex=m_screenTex; }
    if (old.valid()) m_device->destroyTexture(old);
}

void HoloPanel::update(float dt) {
    m_clock += dt;
    if (!m_scene || m_pane==kNoLink || m_pane>=m_scene->size()) return;
    const float pulse = 0.90f + 0.10f*m_shimmer*std::sin(m_clock*1.7f);
    const float flick = 0.98f + 0.02f*std::sin(m_clock*13.0f);
    Entity& e = m_scene->get(m_pane);
    e.emissive[0]=m_emBase[0]; e.emissive[1]=m_emBase[1]; e.emissive[2]=m_emBase[2]; e.emissive[3]=m_emBase[3]*pulse*flick;
}

} // namespace x3::game
