#include "ui_gfx.h"
#include "ft6336g.h"   // 粘滞看门狗需复位触摸芯片
#include <TFT_eSPI.h>   // GFX 风格直绘 API (Bodmer), 项目已在用 (Sprite 类也在此库中)
#include <string.h>
#include <stdio.h>
#include <math.h>

extern TFT_eSPI tft;       // 在主程序里实例化
extern FT6336G touchPanel; // 在主程序里实例化

/* ---------- 全局绘制互斥锁 (跨任务 SPI 保护) ----------
 * 用递归锁: uiScanClear() 内部会调 uiShowList(), 非递归锁会死锁 */
static SemaphoreHandle_t uiMux = NULL;
static void uiLock(void){
    if(!uiMux) uiMux = xSemaphoreCreateRecursiveMutex();
    if(uiMux) xSemaphoreTakeRecursive(uiMux, portMAX_DELAY);
}
static void uiUnlock(void){
    if(uiMux) xSemaphoreGiveRecursive(uiMux);
}

/* ---------- 颜色 ---------- */
static const uint16_t BLK  = 0x0000, WHT = 0xFFFF;
static const uint16_t ORNG = tft.color565(255,149,0);
static const uint16_t CYAN = tft.color565(0,229,255);
static const uint16_t BLUE = tft.color565(30,136,255);
static const uint16_t RED  = tft.color565(255,59,48);
static const uint16_t GRN  = tft.color565(52,199,89);
static const uint16_t YEL  = tft.color565(255,204,0);
static const uint16_t MAG  = tft.color565(255,45,85);
static const uint16_t GRY  = tft.color565(158,158,158);
static const uint16_t TRK  = tft.color565(42,42,42);
static const uint16_t CARD = tft.color565(30,30,30);
static const uint16_t TEAL = tft.color565(52,199,169);

enum { P_SCAN, P_LIST, P_DASH, P_INFO };
static uint8_t page = P_SCAN;

/* 经典 5x7 字体: 字符宽=6*sz 高=8*sz, 绘制最快 */
static int tw(const char *s, uint8_t sz){ return (int)strlen(s) * 6 * sz; }
static void ptext(const char *s, int x, int y, uint16_t c, uint8_t sz){
    tft.setTextColor(c); tft.setTextSize(sz); tft.setTextWrap(false, false);
    tft.setCursor(x, y); tft.print(s);
}
/* 右对齐字段: 值不变直接返回; 变了只擦这一小块 */
static void fieldR(int xr, int y, int w, int h, const char *s, uint16_t c,
                   uint8_t sz, char *cache){
    if (cache && strcmp(cache, s) == 0) return;
    if (cache) strcpy(cache, s);
    tft.fillRect(xr - w, y, w, h, BLK);
    ptext(s, xr - tw(s, sz), y + (h - 8 * sz) / 2 + 1, c, sz);
}
static void centerTxt(const char *s, int y, uint16_t c, uint8_t sz){
    ptext(s, 120 - tw(s, sz) / 2, y, c, sz);
}

/* ---------- 按钮 ---------- */
typedef struct { int x, y, w, h; const char *txt; uint16_t fg; uint8_t sz; bool outline; bool left; } Btn;
static void drawBtn(const Btn &b, bool inv){
    if (b.outline){
        tft.fillRoundRect(b.x, b.y, b.w, b.h, b.h/2, inv ? b.fg : BLK);
        tft.drawRoundRect(b.x, b.y, b.w, b.h, b.h/2, b.fg);
        ptext(b.txt, b.x + b.w/2 - tw(b.txt,b.sz)/2, b.y + b.h/2 - 4*b.sz,
              inv ? BLK : b.fg, b.sz);
    } else {
        tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, inv ? WHT : b.fg);
        int tx = b.left ? b.x + 8 : b.x + b.w/2 - tw(b.txt,b.sz)/2;  // 左对齐模式
        ptext(b.txt, tx, b.y + b.h/2 - 4*b.sz, inv ? BLK : BLK, b.sz);
    }
}
static bool inBtn(const Btn &b, int x, int y){
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* 每页按钮表 */
static const Btn B_CANCEL = {70,284,100,28,"Cancel",CYAN,2,true,false};
static const Btn B_RESCAN = {150,26,82,26,"Rescan",CYAN,2,true,false};
static const Btn B_CONN   = {16,268,208,40,"Connect",BLUE,2,false,false};
static const Btn B_DIS    = {10,278,65,30,"Dis",TEAL,2,false,true};   // 短按钮+文字左对齐
static const Btn B_BACK   = {156,30,76,28,"Back",CYAN,2,true,false};
static const Btn B_INFO   = {200,278,36,30,"i",GRY,2,true,false};   // 仪表页右下进 Info (放大易触摸)

/* 前向声明: 定义于设备列表节 (fireBtn 先于其使用) */
static int sel = -1;
static void listSelect(int i);

static int hitBtn(int x, int y){
    switch (page){
    case P_SCAN: if (inBtn(B_CANCEL,x,y)) return 1; break;
    case P_LIST: if (inBtn(B_RESCAN,x,y)) return 1;
                 if (inBtn(B_CONN,x,y))   return 2;
                 for (int i=0;i<4;i++){ int cy=88+i*46;
                     if (y>=cy && y<cy+40 && x>=8 && x<232) return 10+i; }
                 break;
    case P_DASH: if (inBtn(B_DIS,x,y))  return 1;
                 if (inBtn(B_INFO,x,y)) return 2; break;
    case P_INFO: if (inBtn(B_BACK,x,y)) return 1; break;
    }
    return 0;
}
static void fireBtn(int id){
    switch (page){
    case P_SCAN: if (id==1 && onUiCancelScan) onUiCancelScan(); break;
    case P_LIST: if (id==1 && onUiRescan) onUiRescan();
                 if (id==2){
                     if (sel<0){               // 没选中设备: 红色闪烁提示, 不再静默
                         uiLock();
                         tft.fillRoundRect(B_CONN.x,B_CONN.y,B_CONN.w,B_CONN.h,8,RED);
                         uiUnlock(); delay(80);
                         uiLock(); drawBtn(B_CONN,false); uiUnlock();
                     } else if (onUiConnect) onUiConnect(sel);
                 }
                 break;  // 卡片: 按下时已选中 (listSelect), fire 无需处理
    case P_DASH: if (id==1 && onUiDisconnect) onUiDisconnect();
                 if (id==2 && onUiOpenInfo) onUiOpenInfo(); break;
    case P_INFO: if (id==1 && onUiBack) onUiBack(); break;
    }
}

/* ---------- 状态栏(每页共用) ---------- */
static void statusBar(void){
    ptext("FarDriver", 8, 4, ORNG, 2);
    tft.fillCircle(198, 12, 4, MAG);
    ptext("BLE", 208, 6, MAG, 1);
}

/* ================= 页面1: 扫描等待 ================= */
static uint32_t spinT = 0; static int16_t spinA = 0;
static void arcSeg(int cx,int cy,int r,int a0,int a1,uint16_t c){
    float px=0,py=0;
    for(int a=a0;a<=a1;a+=6){
        float rd=a*M_PI/180, x=cx+r*cosf(rd), y=cy+r*sinf(rd);
        if(a>a0) tft.drawLine((int)px,(int)py,(int)x,(int)y,c);
        px=x; py=y;
    }
}
void uiShowScan(void){
    page=P_SCAN; uiLock(); tft.startWrite();
    tft.fillScreen(BLK);
    statusBar();
    centerTxt("FarDriver", 44, ORNG, 3);
    centerTxt("Scanning BLE...", 80, WHT, 2);
    for(int i=0;i<7;i++) arcSeg(120,160,38+i,0,360,TRK);   // 轨道画一次
    centerTxt("Looking for controller", 224, GRY, 2);
    drawBtn(B_CANCEL,false);
    spinA=0; spinT=millis();
    tft.endWrite(); uiUnlock();
}
static void scanTick(void){
    if (millis()-spinT < 50) return; spinT=millis();
    // 预渲染轨道圈到 Sprite (避免每帧重绘56条线段)
    static TFT_eSprite *trackSprite = nullptr;
    if (!trackSprite) {
        trackSprite = new TFT_eSprite(&tft);
        trackSprite->setColorDepth(16);
        if (trackSprite->createSprite(94, 94)) {
            trackSprite->fillSprite(BLK);
            for(int i=0;i<7;i++) arcSeg(77,77,38+i,0,360,TRK);
        }
    }
    tft.startWrite();
    // 擦旧弧 (黑色直接画到屏幕)
    arcSeg(120,160,41,spinA,spinA+66,BLK);
    // 推 Sprite 重绘轨道圈 (使用黑色透明色, 覆盖擦除区域)
    if (trackSprite && trackSprite->getPointer()) {
        trackSprite->pushSprite(73, 113, BLK);  // BLK 作为透明色
    } else {
        // Sprite 失败则回退到原始方式
        for(int i=0;i<7;i++) arcSeg(120,160,38+i,0,360,TRK);
    }
    spinA=(spinA+24)%360;
    // 画新弧
    for(int i=0;i<7;i++) arcSeg(120,160,38+i,spinA,spinA+60,ORNG);
    tft.endWrite();
}

/* ================= 页面2: 设备列表 ================= */
#define MAXDEV 8
static char devName[MAXDEV][20]; static int8_t devRssi[MAXDEV];
static int devCnt=0;
void (*onUiConnect)(int)=NULL; void (*onUiRescan)(void)=NULL;
void (*onUiCancelScan)(void)=NULL; void (*onUiDisconnect)(void)=NULL;
void (*onUiBack)(void)=NULL; void (*onUiOpenInfo)(void)=NULL;

static void drawCard(int i){
    int y=88+i*46;
    tft.fillRect(8,y,224,40,CARD);
    if (i==sel){
        tft.fillRect(8,y,4,40,ORNG);
        tft.drawRect(8,y,224,40,WHT);   // 选中白框常驻 (替代临时按下框, 不再闪烁)
    }
    // 截断过长设备名 (最多12字符+省略号, 防止与RSSI重叠)
    char nameBuf[16];
    if (strlen(devName[i]) > 12) {
        strncpy(nameBuf, devName[i], 12);
        nameBuf[12] = '\0';
        strcat(nameBuf, "...");
    } else {
        strcpy(nameBuf, devName[i]);
    }
    ptext(nameBuf, 20, y+6, WHT, 2);
    char r[10]; sprintf(r,"%ddBm",devRssi[i]);
    ptext(r, 224-tw(r,1), y+4, CYAN, 1);
    int lv = devRssi[i]>-55?4: devRssi[i]>-70?3: devRssi[i]>-85?2:1;
    for(int b=0;b<4;b++)
        tft.fillRect(224-8-(4-b)*6, y+34-(6+3*b), 4, 6+3*b, b<lv?GRN:TRK);
}
static void listSelect(int i){
    if(i>=devCnt) return;
    int old=sel; sel=i;
    uiLock();
    tft.startWrite();
    if(old>=0) drawCard(old);
    drawCard(sel);
    drawBtn(B_CONN,false);
    tft.endWrite();
    uiUnlock();
}
void uiScanClear(void){ uiLock(); devCnt=0; sel=-1; if(page==P_LIST){ uiShowList(); } uiUnlock(); }
void uiScanAdd(const char *n, int8_t r){
    if(devCnt>=MAXDEV) return;
    strncpy(devName[devCnt],n,19); devRssi[devCnt]=r; devCnt++;
    if(sel<0) sel=0;                          // 自动选中第一台, Connect 不再无响应
    if(page==P_LIST){ uiLock(); tft.startWrite(); drawCard(devCnt-1);
        char f[20]; sprintf(f,"found %d devices",devCnt);
        fieldR(232,64,224,16,f,GRY,2,NULL); tft.endWrite(); uiUnlock(); }
}
void uiShowList(void){
    page=P_LIST; uiLock(); tft.startWrite();
    tft.fillScreen(BLK);
    statusBar();
    ptext("FarDriver", 10, 28, ORNG, 3);
    drawBtn(B_RESCAN,false);
    tft.fillRect(8,58,224,2,TRK);
    char f[20]; sprintf(f,"found %d devices",devCnt);
    ptext(f,10,64,GRY,2);
    for(int i=0;i<4;i++){
        if(i<devCnt) drawCard(i);
        else tft.fillRect(8,88+i*46,224,40,CARD);
    }
    drawBtn(B_CONN,false);
    tft.endWrite(); uiUnlock();
}

/* ================= 页面3: 仪表 ================= */
static int16_t ptrA = -1;
static char cSpd[12],cV[10],cA[10],cKW[10],cCtr[10],cMot[10],cGear[8],cThr[8],cBat[8];
static uint8_t lastThr=255,lastBat=255;
static int spdMaxW = 0;  // 速度文本历史最大宽度 (防止位数减少时残影)
static uint32_t lastSlowMs = 0;  // 慢字段刷新节流 (SPI 干扰缓解: 拧转把时减半写屏活动)

static void gaugeStatic(void){
    const uint16_t cols[8]={BLUE,CYAN,tft.color565(47,217,128),
        tft.color565(126,211,33),YEL,ORNG,
        tft.color565(255,106,0),RED};
    for(int s=0;s<8;s++){                       // 渐变色带,只画一次
        int a0=135+s*34+2, a1=135+(s+1)*34-2; if(a1>405)a1=405;
        for(int r=74;r<=80;r++) arcSeg(120,112,r,a0,a1,cols[s]);
    }
    for(int a=135;a<=405;a+=9){                 // 刻度,只画一次
        bool mj=((a-135)%27==0);
        float rd=a*M_PI/180;
        int r1=mj?62:66, r2=70;
        tft.drawLine(120+r1*cosf(rd),112+r1*sinf(rd),
                     120+r2*cosf(rd),120*0+112+r2*sinf(rd), mj?WHT:GRY);
    }
    centerTxt("km/h",132,GRY,2);
}
static void drawPtr(int ang,uint16_t c){
    float rd=ang*M_PI/180;
    // 用 drawWideLine 画粗指针 (3像素宽, 视觉更清晰)
    tft.drawWideLine(120+58*cosf(rd),112+58*sinf(rd),
                     120+82*cosf(rd),112+82*sinf(rd),3.0f,c);
}
void uiShowDash(void){
    page=P_DASH; uiLock(); tft.startWrite();
    tft.fillScreen(BLK);
    statusBar(); gaugeStatic();
    /* 静态标签 (整体上移20px, 为底部按钮腾出空间) */
    ptext("V",26,184,RED,2);   ptext("A",26,202,CYAN,2);  ptext("kW",26,220,BLUE,2);
    ptext("Ctr",128,184,ORNG,2); ptext("Mot",128,202,YEL,2); ptext("Gear",128,220,GRN,2);
    ptext("Thr",10,240,WHT,2); ptext("Bat",10,260,WHT,2);
    tft.fillRect(46,242,146,12,TRK); tft.fillRect(46,262,146,12,TRK);
    drawBtn(B_DIS,false); drawBtn(B_INFO,false);
    ptrA=-1; memset(cSpd,0xFF,sizeof(cSpd));   /* 强制首帧全画 */
    spdMaxW = 0;  // 重置速度最大宽度追踪
    memset(cV,0xFF,sizeof(cV)); memset(cA,0xFF,sizeof(cA)); memset(cKW,0xFF,sizeof(cKW));
    memset(cCtr,0xFF,sizeof(cCtr)); memset(cMot,0xFF,sizeof(cMot));
    memset(cGear,0xFF,sizeof(cGear)); memset(cThr,0xFF,sizeof(cThr)); memset(cBat,0xFF,sizeof(cBat));
    lastThr=lastBat=255;
    tft.endWrite(); uiUnlock();
}
void uiDashUpdate(const DashData *d){
    uiLock(); tft.startWrite();
    char s[12];
    /* 快字段: 速度数字 + 指针 (用户核心感知, 保持 100ms 流畅) */
    sprintf(s,"%.1f",d->speed);
    { int w = tw(s,4); if(w>spdMaxW) spdMaxW=w; }  // 追踪速度文本历史最大宽度
    fieldR(192,96,spdMaxW,34,s,WHT,4,cSpd);   // 动态宽度, 防止位数减少时残影
    int ang=135+(int)((d->speed>80?80:d->speed)/80.0f*270);
    if(ang!=ptrA){ if(ptrA>=0) drawPtr(ptrA,BLK); drawPtr(ang,WHT); ptrA=ang; }

    /* 慢字段: 300ms 节流 — 拧转把时 SPI 高频写屏(电磁干扰)导致触摸芯片报伪点,
     * 降级刷新频率可把 SPI 活动量降到约 1/3, 大幅缩短干扰窗口
     * (首帧 cSpd=0xFF 时强制全画, 页面切换后不留黑区) */
    if(cSpd[0]==0xFF || millis()-lastSlowMs >= 300){
        lastSlowMs = millis();
        sprintf(s,"%.1f",d->volt);  fieldR(110,184,52,16,s,WHT,2,cV);
        sprintf(s,"%.1f",d->curr);  fieldR(110,202,52,16,s,WHT,2,cA);
        sprintf(s,"%.2f",d->power); fieldR(110,220,52,16,s,WHT,2,cKW);
        sprintf(s,"%dC",d->ctr);    fieldR(232,184,52,16,s,WHT,2,cCtr);
        sprintf(s,"%dC",d->mot);    fieldR(232,202,52,16,s,WHT,2,cMot);
        sprintf(s,"%d",d->gear);    fieldR(232,220,52,16,s,WHT,2,cGear);
        sprintf(s,"%d%%",d->thr);   fieldR(232,240,40,16,s,WHT,2,cThr);
        sprintf(s,"%d%%",d->bat);   fieldR(232,260,40,16,s,WHT,2,cBat);
        if(d->thr!=lastThr){ lastThr=d->thr;
            tft.fillRect(46,242,146,12,TRK); tft.fillRect(46,242,146*d->thr/100,12,ORNG); }
        if(d->bat!=lastBat){ lastBat=d->bat;
            tft.fillRect(46,262,146,12,TRK);
            tft.fillRect(46,262,146*d->bat/100,12, d->bat<20?RED:GRN); }
    }
    tft.endWrite(); uiUnlock();
}

/* ================= 页面4: Info ================= */
static char iRpm[12],iSpd[12],iPwr[12],iVol[12],iMot[8],iCtr[8],iCur[10],iGear[8];
static char cRv[10],cRkw[10],cPole[8],cMrpm[8],cHw[12],cSw[12],cTot[14];
static int totMaxW = 0;  // Total 文本历史最大宽度 (防止位数减少时残影)
/* 快速检查 Info 数据是否有变化 (不加锁, 纯数值比较, 短路返回) */
static bool infoDataChanged(const LiveInfo *l, const CfgInfo *c){
    char t[14];
    sprintf(t,"%.0f",l->rpm);   if(strcmp(t,iRpm)) return true;
    sprintf(t,"%.1f",l->speed); if(strcmp(t,iSpd)) return true;
    sprintf(t,"%.2f",l->power); if(strcmp(t,iPwr)) return true;
    sprintf(t,"%.1f",l->volt);  if(strcmp(t,iVol)) return true;
    sprintf(t,"%.0fC",l->mot);  if(strcmp(t,iMot)) return true;
    sprintf(t,"%.0fC",l->ctr);  if(strcmp(t,iCtr)) return true;
    sprintf(t,"%.1fA",l->curr); if(strcmp(t,iCur)) return true;
    sprintf(t,"%d",l->gear);    if(strcmp(t,iGear)) return true;
    if(c){
        sprintf(t,"%.1fV",c->ratedV);  if(strcmp(t,cRv)) return true;
        sprintf(t,"%u",c->pole);       if(strcmp(t,cPole)) return true;
        sprintf(t,"%u",c->maxRpm);    if(strcmp(t,cMrpm)) return true;
        sprintf(t,"%.1fkW",c->ratedKW);if(strcmp(t,cRkw)) return true;
        if(strcmp(c->hw,cHw)) return true;
        if(strcmp(c->sw,cSw)) return true;
        sprintf(t,"%.1f km",c->totalKm);if(strcmp(t,cTot)) return true;
    }
    return false;
}
void uiShowInfo(void){
    page=P_INFO; uiLock(); tft.startWrite();
    tft.fillScreen(BLK);
    statusBar();
    ptext("Info",10,30,ORNG,3); drawBtn(B_BACK,false);
    tft.fillRoundRect(8,58,224,110,8,CARD);
    ptext("LIVE DATA",18,64,CYAN,2);
    /* 缩短标签防止溢出到数值区域 */
    ptext("RPM",18,88,MAG,2);   ptext("Mot",128,88,YEL,2);
    ptext("Spd",18,106,CYAN,2); ptext("Ctr",128,106,ORNG,2);
    ptext("Pwr",18,124,BLUE,2); ptext("Cur",128,124,CYAN,2);
    ptext("Volt",18,142,RED,2); ptext("Gear",128,142,GRN,2);
    tft.fillRoundRect(8,174,224,88,8,CARD);
    ptext("CONFIG",18,180,ORNG,2);
    ptext("RV",18,202,RED,2);    ptext("R.kW",128,202,BLUE,2);
    ptext("Pole",18,220,GRY,2);  ptext("HW",128,220,GRY,2);
    ptext("MRPM",18,238,MAG,2);  ptext("SW",128,238,GRY,2);
    tft.fillRoundRect(8,268,224,42,8,CARD);
    ptext("Dist",18,280,ORNG,2);
    memset(iRpm,0xFF,sizeof(iRpm)); memset(cTot,0xFF,sizeof(cTot)); /* 首帧强刷 */
    totMaxW = 0;  // 重置 Total 最大宽度追踪
    memset(iSpd,0xFF,sizeof(iSpd)); memset(iPwr,0xFF,sizeof(iPwr)); memset(iVol,0xFF,sizeof(iVol));
    memset(iMot,0xFF,sizeof(iMot)); memset(iCtr,0xFF,sizeof(iCtr)); memset(iCur,0xFF,sizeof(iCur)); memset(iGear,0xFF,sizeof(iGear));
    memset(cRv,0xFF,sizeof(cRv)); memset(cRkw,0xFF,sizeof(cRkw)); memset(cPole,0xFF,sizeof(cPole));
    memset(cMrpm,0xFF,sizeof(cMrpm)); memset(cHw,0xFF,sizeof(cHw)); memset(cSw,0xFF,sizeof(cSw));
    tft.endWrite(); uiUnlock();
}
void uiInfoUpdate(const LiveInfo *l, const CfgInfo *c){
    if (!infoDataChanged(l, c)) return;   // 数据没变, 不拿锁 → 触摸不被阻塞
    uiLock(); tft.startWrite(); char s[14];
    sprintf(s,"%.0f",l->rpm);   fieldR(112,88,56,16,s,WHT,1,iRpm);   // 字号从2降到1
    sprintf(s,"%.1f",l->speed); fieldR(112,106,56,16,s,WHT,1,iSpd);
    sprintf(s,"%.2f",l->power); fieldR(112,124,56,16,s,WHT,1,iPwr);
    sprintf(s,"%.1f",l->volt);  fieldR(112,142,56,16,s,WHT,1,iVol);
    sprintf(s,"%.0fC",l->mot);  fieldR(222,88,56,16,s,WHT,1,iMot);
    sprintf(s,"%.0fC",l->ctr);  fieldR(222,106,56,16,s,WHT,1,iCtr);
    sprintf(s,"%.1fA",l->curr); fieldR(222,124,56,16,s,WHT,1,iCur);
    sprintf(s,"%d",l->gear);    fieldR(222,142,56,16,s,WHT,1,iGear);
    if(c){
        sprintf(s,"%.1fV",c->ratedV);  fieldR(112,202,56,16,s,WHT,1,cRv);   // 字号从2降到1
        sprintf(s,"%u",c->pole);       fieldR(112,220,56,16,s,WHT,1,cPole);
        sprintf(s,"%u",c->maxRpm);    fieldR(112,238,56,16,s,WHT,1,cMrpm);
        sprintf(s,"%.1fkW",c->ratedKW);fieldR(222,202,56,16,s,WHT,1,cRkw);
        fieldR(222,220,56,16,c->hw,WHT,1,cHw);
        fieldR(222,238,56,16,c->sw,WHT,1,cSw);
        sprintf(s,"%.1f km",c->totalKm);
        { int w = tw(s,2); if(w>totMaxW) totMaxW=w; }  // 追踪 Total 历史最大宽度
        fieldR(222,276,totMaxW,24,s,WHT,2,cTot);   // 字号从3降到2, 动态宽度防残影
    }
    tft.endWrite(); uiUnlock();
}

/* ---------- 初始化 ---------- */
void uiBegin(void){
    page = P_SCAN;
}

/* ---------- 按钮/列表项 按下反馈 ---------- */
static void redrawPressed(int id, bool pressed){
    switch (page){
    case P_SCAN: if (id==1) drawBtn(B_CANCEL,pressed); break;
    case P_LIST:
        if (id==1) drawBtn(B_RESCAN,pressed);
        else if (id==2) drawBtn(B_CONN,pressed);
        /* 卡片: 选中态由 drawCard 常驻渲染 (橙条+白框), 无需临时按下反馈 */
        break;
    case P_DASH: if (id==1) drawBtn(B_DIS,pressed);
                 if (id==2) drawBtn(B_INFO,pressed); break;
    case P_INFO: if (id==1) drawBtn(B_BACK,pressed); break;
    }
}

/* ---------- 主循环: 触摸状态机 + spinner ----------
 * 按下跟踪模式:
 * 1. 按下时 hitBtn 锁定目标, 手指滑动时实时跟踪 (滑到哪个按钮就是哪个)
 * 2. 松开时触发当前锁定的按钮 → 只要松开时还在按钮上就必然响应
 * 3. 卡片按下即选中 (白框常驻), 不再依赖松开才生效
 * 4. Connect 触发后加短冷却, 防止残留触摸连发 */
#define RELEASE_CNT     1          // 连续"未按下"采样次数才算松开 (1=10ms响应, 更灵敏)
#define CONNECT_LOCK_MS 300        // Connect 触发后冷却 (连接是阻塞操作)
#define PRESS_TIMEOUT_MS 6000      // 按下状态超时 (芯片状态粘滞看门狗: 6s无松开→强制复位, 折中: 避免复位风暴吞点击)
#define RESET_COOLDOWN_MS 30000    // 复位冷却: 30s 内只清状态不硬件复位 (防复位风暴)
static int pressId=-1; static uint32_t touchT=0;
static uint8_t relCnt=0;           // 连续未按下计数
static bool pressWasNew=false;     // 锁定点是否为新出现 (新点=手指, 旧点=伪点, 防误触发)
static int16_t lastTx=-1, lastTy=-1; // 上次采样选中坐标 (漂移累计用)
static uint32_t moveAcc=0;         // 锁定期间累计位移 (漂移伪点过滤: 手指点按位移极小)
static uint32_t connectLock=0;     // Connect 冷却计时
static uint32_t pressSince=0;      // 按下开始时间 (粘滞看门狗)
static uint32_t lastReset=0;       // 上次芯片复位时间 (复位冷却)
static uint32_t s_btnPress=0;      // 按钮锁定累计次数 (失灵检测: 点了没触发=芯片坏了)
static uint32_t s_fireCnt=0;       // fire 触发累计次数 (仅计 fid>0 的真实操作)
static uint32_t s_driftCnt=0;      // 漂移解锁累计次数 (漂移风暴检测: 伪点占满 2 点容量)

/* 失灵检测统计 (diagTask 周期读取) */
uint32_t uiGetBtnPressCnt(void){ return s_btnPress; }
uint32_t uiGetFireCnt(void){ return s_fireCnt; }
uint32_t uiGetDriftCnt(void){ return s_driftCnt; }

void uiLoop(void){
    /* Connect 冷却: 连接是阻塞操作(最长5s), 防止残留触摸连发 */
    if(connectLock){
        if(millis()-connectLock>=CONNECT_LOCK_MS) connectLock=0;
        else return;
    }

    if(millis()-touchT<10) return; touchT=millis();   // 触摸 100Hz 轮询足够
    int16_t x,y; bool pr;
    if(!touchPoll(&x,&y,&pr)) return;                 // 触摸读取永远在锁外

    /* 选中点释放事件: 手指抬起即触发 (不依赖全局无触摸, 伪点常驻也能识别)
     * 需在读后清零的 getter 之后立即使用, 与本次采样一一对应 */
    bool selRel = touchPanel.getSelReleased();
    bool selNew = touchPanel.getSelNew();             // 本次选中点是否新出现

    bool fire=false; int fid=0;
    uiLock();
    if(page==P_SCAN) scanTick();
    tft.startWrite();
    if(pr){
        relCnt=0;
        /* 漂移伪点过滤: BLE 数据流干扰时芯片报双点漂移伪点 (位置持续跳变)
         * 真实手指点按位移极小; 累计位移 >60px → 伪点 → 解锁放弃本次
         * 新点(手指)按下 → 重新累计 (选点切换/手指出现不误伤) */
        if(selNew || !pressSince){ moveAcc=0; lastTx=x; lastTy=y; }
        else {
            if(lastTx>=0){
                int d=(x>lastTx?x-lastTx:lastTx-x)+(y>lastTy?y-lastTy:lastTy-y);
                moveAcc += d;
            }
            lastTx=x; lastTy=y;
            if(moveAcc > 60){
                s_driftCnt++;                        // 漂移解锁计数 (风暴检测用)
                static uint32_t lastDriftLog=0;      // 日志限速: 5s 一条, 防刷屏淹没问题
                if(millis()-lastDriftLog>5000){ Serial.println(F("[WD] drift point, cleared")); lastDriftLog=millis(); }
                if(pressId>0) redrawPressed(pressId,false);
                pressId=-1; relCnt=0; pressSince=0; pressWasNew=false; moveAcc=0;
                lastTx=-1; lastTy=-1;
                tft.endWrite(); uiUnlock();
                return;
            }
        }
        /* 粘滞看门狗: 按下持续超时 → 芯片状态卡死, 强制清空并复位
         * 复位冷却: 30s 内再次超时只清状态, 不反复硬件复位 (防复位风暴) */
        if(!pressSince) pressSince=millis();
        else if(millis()-pressSince >= PRESS_TIMEOUT_MS){
            pressSince=0;
            if(pressId>0){ redrawPressed(pressId,false); pressId=-1; }  // 先恢复按钮
            relCnt=0; pressWasNew=false;
            tft.endWrite(); uiUnlock();
            if(millis()-lastReset >= RESET_COOLDOWN_MS){
                lastReset=millis();
                Serial.println(F("[WD] touch stuck, resetting chip"));
                touchPanel.recover();                // 复位芯片 (锁外执行, 有冷却)
            } else {
                Serial.println(F("[WD] touch stuck, state cleared"));
            }
            return;
        }
        int cur=hitBtn(x,y);
        if(cur != pressId){                  // 新按下 或 滑动切换到其他按钮
            if(pressId>0) redrawPressed(pressId,false);
            if(page==P_LIST && cur>=10 && cur-10<devCnt){
                listSelect(cur-10);             // 卡片按下即选中
                s_fireCnt++;                    // 选中即成功响应 (列表页也享失灵兜底)
            }
            pressId=cur;
            pressWasNew = selNew;            // 记录本次锁定点是否手指(新点)
            if(pressId>0){ redrawPressed(pressId,true); s_btnPress++; }  // 按钮锁定计数
        } else if (selNew) {
            pressWasNew = true;              // 同一按钮上的新按压 (伪点已锁定时手指补按)
        }
    } else if(pressId>=0){
        lastTx=-1; lastTy=-1; moveAcc=0;
        if(++relCnt>=RELEASE_CNT){          // 全局松开: 触发当前锁定按钮
            fid=pressId; pressId=-1; relCnt=0; pressSince=0; pressWasNew=false;
            if(fid>0) redrawPressed(fid,false);
            fire=true;
        }
    }
    if(selRel && pressId>=0 && pressWasNew && !fire){
        /* 选中点释放 (伪点还赖在屏幕上, 但手指已抬起) → 触发
         * 仅限"新出现"的锁定点: 伪点常驻时其消失不得误触发 */
        if(++relCnt>=RELEASE_CNT){
            fid=pressId; pressId=-1; relCnt=0; pressSince=0; pressWasNew=false;
            if(fid>0) redrawPressed(fid,false);
            fire=true;
        }
    }
    tft.endWrite();
    uiUnlock();

    if(fire){
        if(fid>0) s_fireCnt++;                   // fire 计数 (仅真实操作, 空白处 fid=0 不计数)
        if(fid==2 && page==P_LIST) connectLock=millis();   // 仅 Connect 加冷却
        fireBtn(fid);                            // 阻塞调用(连接)放到锁外
    }
}
