#include "cbase.h"
#include "mobile_gameui.h"
#include "c_cs_player.h"
#include "c_plantedc4.h"
#include "cs_gamerules.h"
#include "weapon_csbase.h"
#include "weapon_csbasegun.h"
#include "weapon_c4.h"
#include "appframework/ilaunchermgr.h"
#include "panorama/controls/label.h"
#include "panorama/controls/image.h"
#include "panorama/layout/uilength.h"
#include "IGameUIFuncs.h"
#include "in_buttons.h"
#include "usercmd.h"
#include "tier0/icommandline.h"
#include "filesystem.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <tier0/memdbgon.h>

using namespace panorama;
using namespace mobile;
REGISTER_PANEL2D_FACTORY( CMobileGameUI, MobileGameUI );

namespace
{
IMobileInputSource *s_input=nullptr;
CMobileGameUI *s_ui=nullptr;
ConVar cl_mobile_context("cl_mobile_context","0",FCVAR_USERINFO,"Request owner-only context hints for the touch HUD.");
enum { MOBILE_RENDER_SCALE_MIN = 50, MOBILE_RENDER_SCALE_MAX = 100, MOBILE_RENDER_SCALE_DEFAULT = 75 };
struct SettingDef { const char *key, *label, *cvar; float min,max,step,defaultValue; };
const SettingDef kSettings[CMobileGameUI::SettingCount]={
    {"volume","主音量","volume",0,1,.05f,.8f},
    {"music","菜单音乐","snd_menumusic_volume",0,1,.05f,.15f},
    {"fps","帧率上限","fps_max",30,120,30,60},
    {"textures","纹理质量","mat_picmip",0,2,1,1},
    {"filter","纹理过滤","mat_forceaniso",0,4,1,2},
    {"shadows","阴影质量","csm_quality_level",0,3,1,1},
    {"sensitivity","普通视角灵敏度",nullptr,.25f,3,.05f,1},
    {"invert","反转垂直视角",nullptr,0,1,1,0},
    {"opacity","触屏按钮透明度",nullptr,.25f,1,.05f,1},
    {"touch","显示触屏操作",nullptr,0,1,1,1},
    {"render_scale","3D 渲染比例","mat_viewportscale",MOBILE_RENDER_SCALE_MIN,MOBILE_RENDER_SCALE_MAX,5,MOBILE_RENDER_SCALE_DEFAULT},
    {"net_graph","性能数据","net_graph",0,1,1,0},
    {"gyro_mode","陀螺仪",nullptr,0,2,1,0},
    {"gyro_sensitivity","陀螺仪灵敏度",nullptr,.25f,4,.05f,1},
    {"scoped_sensitivity","开镜视角灵敏度",nullptr,.05f,3,.05f,.5f}
};
const int kSensitivitySetting=6;
const int kRenderScaleSetting=10;
const int kGyroModeSetting=12;
const int kGyroSensitivitySetting=13;
const int kScopedSensitivitySetting=14;
CUILength Percent(float value){return CUILength(value,CUILength::k_EUILengthPercent);}
CUILength Pixels(float value){return CUILength(value,CUILength::k_EUILengthLength);}
bool ValidSetting(int i) { return i>=0 && i<CMobileGameUI::SettingCount; }
float CleanSetting(int i,float value)
{
    const SettingDef &s=kSettings[i];
    if(!Finite(value)) value=s.defaultValue;
    value=Clamp(value,s.min,s.max);
    return Clamp(s.min+std::round((value-s.min)/s.step)*s.step,s.min,s.max);
}
void SetVisibleChild(CPanel2D *root,const char *id,bool value)
{
    if(CPanel2D *p=root->FindChildInLayoutFile(id)) p->SetVisible(value);
}
void SetTextChild(CPanel2D *root,const char *id,const char *text)
{
    if(CLabel *p=panel_cast<CLabel *>(root->FindChildInLayoutFile(id))) p->SetText(s_ui?s_ui->LocalizeText(text):text);
}
bool SafeMapName(const char *name)
{
    if(!name || !*name || V_strlen(name)>96) return false;
    for(const char *p=name; *p; ++p)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-')) return false;
    return true;
}
int WeaponControl(CWeaponCSBase *weapon)
{
    if(!weapon)return -1;
    const CSWeaponID id=weapon->GetCSWeaponID();
    if(id==WEAPON_TASER)return Taser;
    if(id==WEAPON_C4)return Objective;
    if(id==WEAPON_HEGRENADE)return HEGrenade;
    if(id==WEAPON_SMOKEGRENADE)return Smoke;
    if(id==WEAPON_FLASHBANG)return Flash;
    if(id==WEAPON_MOLOTOV)return Molotov;
    if(id==WEAPON_INCGRENADE)return Incendiary;
    if(id==WEAPON_DECOY)return Decoy;
    if(id==WEAPON_HEALTHSHOT)return Healthshot;
    if(id==WEAPON_TAGRENADE)return TagGrenade;
    if(id==WEAPON_BREACHCHARGE)return BreachCharge;
    if(id==WEAPON_SNOWBALL)return Snowball;
    if(id==WEAPON_TABLET)return Tablet;
    if(id==WEAPON_FISTS)return Fists;
    if(id==WEAPON_MELEE)return Melee;
    if(id==WEAPON_FIREBOMB)return FireBomb;
    if(id==WEAPON_FRAGGRENADE)return FragGrenade;
    if(id==WEAPON_DIVERSION)return Diversion;
    const char *name=weapon->GetClassname();
    if(!V_strcmp(name,"weapon_shield"))return Shield;
    if(!V_strcmp(name,"weapon_bumpmine"))return BumpMine;
    switch(weapon->GetWeaponType())
    {
    case WEAPONTYPE_PISTOL:return Pistol;
    case WEAPONTYPE_KNIFE:return Knife;
    case WEAPONTYPE_RIFLE:case WEAPONTYPE_SNIPER_RIFLE:case WEAPONTYPE_SUBMACHINEGUN:
    case WEAPONTYPE_SHOTGUN:case WEAPONTYPE_MACHINEGUN:return Primary;
    default:return -1;
    }
}
CWeaponCSBase *OwnedWeapon(int entity,uint32_t token)
{
    C_CSPlayer *player=C_CSPlayer::GetLocalCSPlayer();
    CWeaponCSBase *weapon=dynamic_cast<CWeaponCSBase *>(ClientEntityList().GetBaseEntity(entity));
    return player&&weapon&&weapon->GetPlayerOwner()==player&&weapon->GetRefEHandle().ToInt()==token ? weapon : nullptr;
}
Button LegacyButton(int i,int version,float aspect)
{
    const Button first[17]={
        {.15f,.73f,.29f,.50f},{.88f,.52f,.15f,.70f},{.77f,.36f,.11f,.60f},
        {.91f,.78f,.12f,.65f},{.78f,.81f,.11f,.60f},{.66f,.73f,.105f,.60f},
        {.66f,.51f,.10f,.60f},{.31f,.82f,.10f,.55f},{.89f,.29f,.10f,.55f},
        {.40f,.90f,.09f,.55f},{.48f,.90f,.09f,.55f},{.56f,.90f,.09f,.55f},
        {.70f,.91f,.09f,.55f},{.47f,.09f,.09f,.50f},{.13f,.39f,.10f,.60f},
        {.93f,.08f,.09f,.65f},{.06f,.09f,.09f,.55f}};
    if(version<2)return first[i];
    const Button second[17]={
        {.10f,.80f,.36f,.55f},{.95f,.54f,.20f,.75f},{.86f,.35f,.145f,.65f},
        {.95f,.90f,.16f,.70f},{.86f,.91f,.145f,.65f},{.78f,.91f,.14f,.65f},
        {.78f,.68f,.135f,.65f},{.24f,.92f,.13f,.60f},{.95f,.29f,.135f,.60f},
        {.38f,.92f,.12f,.60f},{.47f,.92f,.12f,.60f},{.56f,.92f,.12f,.60f},
        {.65f,.92f,.12f,.60f},{.50f,.07f,.12f,.55f},{.04f,.46f,.14f,.65f},
        {.96f,.07f,.12f,.70f},{.04f,.07f,.12f,.60f}};
    Button b=second[i];const float edge=(b.size*.5f+.012f)/aspect;
    if(i==mobile::Move||i==Buy||i==Team)b.x=edge;
    if(i==Fire||i==Jump||i==8||i==Menu)b.x=1-edge;
    if(i==Aim||i==Duck)b.x=1-edge-.19f/aspect;
    if(i==Reload||i==Use)b.x=1-edge-.38f/aspect;
    if(i==Walk)b.x=(.36f+.06f+b.size*.5f)/aspect;
    if(i==mobile::Move||i==Jump||i==Duck||i==Reload||i==Walk||(i>=Primary&&i<=HEGrenade))b.y=1-b.size*.5f-.012f;
    if(i==Score||i==Menu||i==Team)b.y=b.size*.5f+.012f;
    return b;
}
}

bool MobileUIEnabled() { return s_input && getenv("CSGO_MOBILE_UI_PATH") && !CommandLine()->FindParm("-nomobileui"); }
void MobileUIInitialize(CreateInterfaceFn factory)
{
    ILauncherMgr *launcher=static_cast<ILauncherMgr *>(factory(SDLMGR_INTERFACE_VERSION,nullptr));
    s_input=launcher ? static_cast<IMobileInputSource *>(launcher->QueryInterface(MOBILE_INPUT_INTERFACE_VERSION)) : nullptr;
}
void MobileUIFrame() { if(s_ui) s_ui->Frame(); }
void MobileUIMove(CUserCmd *cmd) { if(s_ui) s_ui->MoveInput(cmd); }
unsigned int MobileUIButtons(CUserCmd *cmd) { return s_ui ? s_ui->Buttons(cmd) : 0; }
void MobileUILoadProgress(float value,const char *status) { if(s_ui) s_ui->Progress(value,status); }
void MobileUILoadFailure(const char *reason) { if(s_ui) s_ui->Failure(reason); }

CMobileGameUI::CMobileGameUI(CPanel2D *parent,const char *id)
    : CUI_Root(parent,id), m_stick(nullptr), m_page(Home), m_revision(0), m_map(0),m_mode(0),m_bots(8),m_difficulty(1),m_team(3),m_selectedControl(mobile::Move),
      m_pendingMatch(false),m_showingScore(false),m_editorFingerActive(false),m_playingLastFrame(false),m_pointerMenus(0),m_editorFinger(0),m_editorX(0),m_editorY(0),m_denyInput(0),m_gameViewport{1920,1080,0,0,1,1},m_layoutWindowW(-1),m_layoutWindowH(-1),m_layoutSurfaceW(-1),m_layoutSurfaceH(-1),m_forceControlLayout(30)
{
    s_ui=this;
    m_englishStrings=new KeyValues("MobileEnglish");
    m_englishStrings->UsesEscapeSequences(true);
    ConVarRef language("cl_language");
    m_englishUI=V_strcmp(language.GetString(),"schinese")&&V_strcmp(language.GetString(),"tchinese");
    if(m_englishUI&&getenv("CSGO_MOBILE_UI_PATH"))
        m_englishStrings->LoadFromFile(g_pFullFileSystem,CFmtStr("%s/panorama/mobile/menu_english.txt",getenv("CSGO_MOBILE_UI_PATH")),nullptr);
    m_playerState={};m_playerState.activeControl=-1;m_interactionState={};m_hud=BuildHud(m_playerState,m_interactionState);
    m_playerToken=m_pendingWeaponToken=0;m_playerTeam=m_pendingWeaponEntity=0;
    m_duckToggle=m_walkToggle=m_wasAlive=false;
    for(int i=0;i<SettingCount;++i) m_settings[i]=m_draft[i]=kSettings[i].defaultValue;
    for(int i=0;i<ControlCount;++i)
    {
        m_controls[i]=nullptr;
        m_icons[i]=nullptr;m_captions[i]=m_counts[i]=nullptr;m_progressTracks[i]=m_progressFills[i]=nullptr;
        m_weaponIconTokens[i]=0;
        m_controlRects[i]={-1,-1,-1,-1};
        m_controlOpacity[i]=-1;
    }
    m_stickX=m_stickY=-1;
    m_editorToolsVisible=true;
    if(s_input)m_touch.SetViewport(s_input->GetTouchViewport());
    m_touch.Defaults();
    const char *local=getenv("USRLOCALCSGO");
    m_configPath=CFmtStr("%s/cfg/mobile_ui.vdf",local?local:"csgo/local");
    m_languagePath=CFmtStr("%s/cfg/language.txt",local?local:"csgo/local");
    ReadGameLanguage();
    ScanMaps(); ReadConfig(); ApplySettings();
    RequireLoadLayout("file://{resources}/mobile/menu.xml");
    TranslateLabels(this);
    CPanel2D *touchRoot=FindChildInLayoutFile("TouchControls");
    for(int i=0;touchRoot&&i<ControlCount;++i)
    {
        CPanel2D *panel=new CPanel2D(touchRoot,CFmtStr("Touch%d",i));
        panel->BLoadLayoutSnippet("TouchButton");panel->AddClass("touch");
        panel->SetHasClass("inventory",IsInventoryControl(i));
        panel->SetHasClass("weapon-card",ControlWidthRatio(i)>1.f);
        panel->SetHasClass("fire",i==Fire||i==FireLeft);
        panel->SetHasClass("movement",i==mobile::Move);
        panel->SetHasClass("utility",IsGrenadeControl(i));
        panel->SetHasClass("objective",i==Objective||i==Use);
        panel->AddClass(Definition(i).key);
        m_controls[i]=panel;
        m_icons[i]=panel_cast<CImagePanel *>(panel->FindChildTraverse("Icon"));
        m_captions[i]=panel_cast<CLabel *>(panel->FindChildTraverse("Caption"));
        m_counts[i]=panel_cast<CLabel *>(panel->FindChildTraverse("Count"));
        m_progressTracks[i]=panel->FindChildTraverse("ProgressTrack");
        m_progressFills[i]=panel->FindChildTraverse("ProgressFill");
    }
    if(m_controls[mobile::Move])m_stick=new CPanel2D(m_controls[mobile::Move],"StickKnob");
    SetAcceptsFocus(true);
    SetInputNamespace("CSGO_mainmenu");
    GameUI().RegisterGameUIStateListener(this);
    OnCSGOGameUIStateChange(CSGO_GAME_UI_STATE_INVALID,GameUI().GetGameUIState());
    Msg("MOBILE_UI_READY: Panorama menu, settings and touch controls\n");
}
CMobileGameUI::~CMobileGameUI()
{
    ResetInput();
    cl_mobile_context.SetValue(0);
    GameUI().UnregisterGameUIStateListener(this);
    if(m_denyInput) gameuifuncs->PanoramaReleaseDenyAllInputToGame(m_denyInput);
    if(s_input) s_input->SetTouchMode(MOBILE_TOUCH_UI);
    s_ui=nullptr;
    m_englishStrings->deleteThis();
}

const char *CMobileGameUI::LocalizeText(const char *text)
{
    if(!text)return "";
    return m_englishUI&&m_englishStrings ? m_englishStrings->GetString(text,text) : text;
}

void CMobileGameUI::TranslateLabels(CPanel2D *panel)
{
    if(!m_englishUI)return;
    if(CLabel *label=panel_cast<CLabel *>(panel))
    {
        const char *original=label->PchGetText();
        const char *translated=LocalizeText(original);
        if(V_strcmp(original,translated))label->SetText(translated);
    }
    for(int i=0;i<panel->GetChildCount();++i)TranslateLabels(panel->GetChild(i));
}

void CMobileGameUI::ReadGameLanguage()
{
    m_gameLanguage=0;
    char saved[32]={};
    if(FILE *file=std::fopen(m_languagePath.String(),"r"))
    {
        const bool read=std::fscanf(file,"%31s",saved)==1;
        std::fclose(file);
        if(read)m_gameLanguage=!V_strcmp(saved,"schinese")?1:!V_strcmp(saved,"english")?2:0;
    }
}

void CMobileGameUI::SetGameLanguage(int language)
{
    if(language<0||language>2)return;
    static const char *codes[]={"auto","schinese","english"};
    char directory[MAX_PATH];V_ExtractFilePath(m_languagePath.String(),directory,sizeof(directory));
    g_pFullFileSystem->CreateDirHierarchy(directory,nullptr);
    CUtlString temporary(CFmtStr("%s.tmp",m_languagePath.String()).Get());
    CUtlBuffer data(0,0,CUtlBuffer::TEXT_BUFFER);data.PutString(codes[language]);data.PutChar('\n');
    if(!g_pFullFileSystem->WriteFile(temporary.String(),nullptr,data)||std::rename(temporary.String(),m_languagePath.String())!=0)
    {
        SetMessage("语言保存失败，请检查配置目录的写入权限");
        return;
    }
    m_gameLanguage=language;
    SetMessage("语言已保存，重新启动游戏后生效");
}

void CMobileGameUI::SetupJavascriptObjectTemplate()
{
    BaseClass::SetupJavascriptObjectTemplate();
#define MOBILE_JS(method) RegisterJSMethod(#method,PANORAMA_DELEGATE(&CMobileGameUI::method))
    MOBILE_JS(Action); MOBILE_JS(GetPage); MOBILE_JS(GetRevision); MOBILE_JS(IsInMatch); MOBILE_JS(GetMessage);
    MOBILE_JS(GetMapCount); MOBILE_JS(GetMapName); MOBILE_JS(GetSelectedMap); MOBILE_JS(GetMode); MOBILE_JS(GetBots); MOBILE_JS(GetDifficulty); MOBILE_JS(GetTeam); MOBILE_JS(StartMatch);
    MOBILE_JS(GetSettingCount); MOBILE_JS(GetSettingLabel); MOBILE_JS(GetSettingMin); MOBILE_JS(GetSettingMax); MOBILE_JS(GetSettingStep); MOBILE_JS(GetSetting); MOBILE_JS(SetSetting);
    MOBILE_JS(GetGameLanguage); MOBILE_JS(SetGameLanguage); MOBILE_JS(LocalizeText);
    MOBILE_JS(GetSelectedControl); MOBILE_JS(SelectControl); MOBILE_JS(GetControlLabel); MOBILE_JS(GetControlSize); MOBILE_JS(GetControlOpacity); MOBILE_JS(SetControlSize); MOBILE_JS(SetControlOpacity);
#undef MOBILE_JS
}

bool CMobileGameUI::IsInMatch() { return engine->IsInGame(); }
bool CMobileGameUI::Playing()
{
    ConVarRef console("console_window_open");
    return GameUI().GetGameUIState()==CSGO_GAME_UI_STATE_INGAME && !Editing() && m_pointerMenus==0
        && !(console.IsValid()&&console.GetBool()) && !gameuifuncs->PanoramaDeniesInputToGame();
}
void CMobileGameUI::AdjustPointerMenu(int delta)
{
    if(!delta) return;
    int next=m_pointerMenus+delta;
    if(next<0) next=0;
    const bool was=m_pointerMenus>0;
    m_pointerMenus=next;
    if(was==(m_pointerMenus>0)) return;
    Msg("MOBILE_UI_POINTER: %s menus=%d\n", m_pointerMenus?"menu":"game", m_pointerMenus);
    ResetInput();
    m_playingLastFrame=Playing();
    UpdateVisibility();
}
void MobileUIAdjustPointerMenu(int delta) { if(s_ui) s_ui->AdjustPointerMenu(delta); }
void CMobileGameUI::SetMessage(const char *message) { m_message=LocalizeText(message); ++m_revision; }
void CMobileGameUI::SetPage(Page page) { ResetInput(); m_page=page; ++m_revision; UpdateVisibility(); }
void CMobileGameUI::ResetInput()
{
    m_touch.Reset(); m_editorFingerActive=false;
    m_duckToggle=m_walkToggle=false;m_pendingWeaponEntity=0;m_pendingWeaponToken=0;
    if(m_showingScore) engine->ClientCmd_Unrestricted("-showscores");
    m_showingScore=false;
}
void CMobileGameUI::UpdateVisibility()
{
    const bool loading=GameUI().GetGameUIState()==CSGO_GAME_UI_STATE_LOADINGSCREEN;
    const bool inGame=GameUI().GetGameUIState()==CSGO_GAME_UI_STATE_INGAME;
    if(inGame && m_denyInput)
    {
        GetParentWindow()->UIWindowInput()->SetInputFocus(nullptr,false,false);
        gameuifuncs->PanoramaReleaseDenyAllInputToGame(m_denyInput);m_denyInput=0;
    }
    const bool playing=Playing();
    const bool pointer=m_pointerMenus>0;
    // Retail loading screen and the HUD's team/buy menus sit under this window.
    // An empty full-screen layer here swallows the clicks those panels need.
    GetParentWindow()->SetVisible(!loading && !pointer);
    SetVisible(!loading && !pointer);
    SetVisibleChild(this,"MenuShell",!inGame&&!loading&&!Editing());
    SetVisibleChild(this,"Editor",Editing());
    SetVisibleChild(this,"EditorToolbar",Editing()&&m_editorToolsVisible);
    SetVisibleChild(this,"ShowEditorTools",Editing()&&!m_editorToolsVisible);
    SetVisibleChild(this,"TouchControls",Editing()||(playing&&m_settings[9]>.5f));
    SetVisibleChild(this,"LoadingPage",false);
    if(!inGame && !loading && !m_denyInput)
    {
        m_denyInput=gameuifuncs->PanoramaAddDenyAllInputToGame(UIPanel(),"MobileMenu");
        SetFocus();
    }
    if(s_input) s_input->SetTouchMode(TouchModeForSession(playing, m_settings[9]>.5f));
}
void CMobileGameUI::OnCSGOGameUIStateChange(CSGOGameUIState_t,CSGOGameUIState_t state)
{
    Msg("MOBILE_UI_STATE: %s time=%.3f\n",CSGOGameUIStateName(state),Plat_FloatTime());
    if(Editing()) for(int i=0;i<ControlCount;++i) m_touch.buttons[i]=m_savedLayout[i];
    m_page=state==CSGO_GAME_UI_STATE_LOADINGSCREEN?Loading:Home;
    ResetInput(); ++m_revision; UpdateVisibility();
}
void CMobileGameUI::OnMapLoadStarted(const char *level,bool)
{
    Msg("MOBILE_MAP_LOAD: start=%s time=%.3f\n",level?level:"",Plat_FloatTime());
    m_loadingMap=level?level:""; SetTextChild(this,"LoadingMap",m_loadingMap.String()); Progress(0,nullptr); ResetInput();
}
void CMobileGameUI::OnMapLoadFinished()
{
    Msg("MOBILE_MAP_LOAD: finished time=%.3f\n",Plat_FloatTime());
    if(m_pendingMatch)
    {
        m_pendingMatch=false;
        engine->ClientCmd_Unrestricted(CFmtStr("bot_quota_mode normal; bot_quota %d; bot_difficulty %d; jointeam %d; gameui_hide",m_bots,m_difficulty,m_team));
    }
    ResetInput();
}
void CMobileGameUI::OnGameLoopDeactivated() { ResetInput(); }

void CMobileGameUI::Action(const char *action)
{
    if(!action) return;
    if(!V_strcmp(action,"home")) { SetMessage(""); SetPage(Home); }
    else if(!V_strcmp(action,"play")) { ScanMaps(); SetMessage(""); SetPage(Play); }
    else if(!V_strcmp(action,"settings")) { ReadLiveSettings(); ReadGameLanguage(); for(int i=0;i<SettingCount;++i)m_draft[i]=m_settings[i]; SetMessage(""); SetPage(Settings); }
    else if(!V_strcmp(action,"layout"))
    {
        m_editorToolsVisible=true;
        for(int i=0;i<ControlCount;++i)m_savedLayout[i]=m_touch.buttons[i];
        SetMessage(""); SetPage(Layout);
    }
    else if(!V_strcmp(action,"toggle_layout_tools")&&Editing())
    {m_editorToolsVisible=!m_editorToolsVisible;ResetInput();UpdateVisibility();}
    else if(!V_strcmp(action,"apply_settings") && m_page==Settings)
    {
        float old[SettingCount]; for(int i=0;i<SettingCount;++i){old[i]=m_settings[i];m_settings[i]=m_draft[i];}
        if(!WriteConfig()) {for(int i=0;i<SettingCount;++i)m_settings[i]=old[i];return;}
        ApplySettings(); engine->ClientCmd_Unrestricted("mat_savechanges; host_writeconfig"); SetMessage("设置已应用并保存"); ++m_revision;
    }
    else if(!V_strcmp(action,"default_settings") && m_page==Settings) {for(int i=0;i<SettingCount;++i)m_draft[i]=kSettings[i].defaultValue; ++m_revision;}
    else if(!V_strcmp(action,"save_layout") && Editing()) {if(WriteConfig()){SetPage(Home);SetMessage("按键布局已保存");}}
    else if(!V_strcmp(action,"default_layout") && Editing()) {m_touch.Defaults();++m_revision;}
    else if(!V_strcmp(action,"resume") && IsInMatch()) {SetPage(Home);GameUI().HideGameUI();}
    else if(!V_strcmp(action,"quit")) SetPage(ConfirmQuit);
    else if(!V_strcmp(action,"confirm_quit") && m_page==ConfirmQuit) engine->ClientCmd_Unrestricted("quit");
    else if(!V_strcmp(action,"disconnect") && IsInMatch()) SetPage(ConfirmDisconnect);
    else if(!V_strcmp(action,"confirm_disconnect") && m_page==ConfirmDisconnect) {m_pendingMatch=false;SetPage(Home);engine->ClientCmd_Unrestricted("disconnect; gameui_activate");}
    else if(!V_strcmp(action,"team_t") && IsInMatch()) {engine->ClientCmd_Unrestricted("jointeam 2; gameui_hide");}
    else if(!V_strcmp(action,"team_ct") && IsInMatch()) {engine->ClientCmd_Unrestricted("jointeam 3; gameui_hide");}
    else if(!V_strcmp(action,"back"))
    {
        if(Editing()) for(int i=0;i<ControlCount;++i)m_touch.buttons[i]=m_savedLayout[i];
        if(m_page!=Home) {SetMessage("");SetPage(Home);}
        else if(IsInMatch()) GameUI().HideGameUI();
        else SetPage(ConfirmQuit);
    }
}

void CMobileGameUI::ScanMaps()
{
    CUtlString selected=GetMapName(m_map);
    m_maps.RemoveAll();
    FileFindHandle_t find;
    const char *file=g_pFullFileSystem->FindFirstEx("maps/*.bsp","GAME",&find);
    for(;file;file=g_pFullFileSystem->FindNext(find))
    {
        if(g_pFullFileSystem->FindIsDirectory(find))continue;
        char name[128]; V_StripExtension(file,name,sizeof(name));
        if(!SafeMapName(name))continue;
        bool duplicate=false; FOR_EACH_VEC(m_maps,i)if(!V_stricmp(m_maps[i],name))duplicate=true;
        if(!duplicate)m_maps.AddToTail(name);
    }
    g_pFullFileSystem->FindClose(find);
    // Deterministic order, with the standard offline map first.
    for(int i=0;i<m_maps.Count();++i) for(int j=i+1;j<m_maps.Count();++j)
        if(!V_strcmp(m_maps[j],"de_dust2") || (V_strcmp(m_maps[i],"de_dust2")&&V_stricmp(m_maps[j],m_maps[i])<0))
        {CUtlString temp=m_maps[i];m_maps[i]=m_maps[j];m_maps[j]=temp;}
    m_map=0; FOR_EACH_VEC(m_maps,i)if(m_maps[i]==selected)m_map=i;
    ++m_revision;
}
const char *CMobileGameUI::GetMapName(int i) {return i>=0&&i<m_maps.Count()?m_maps[i].String():"";}
void CMobileGameUI::StartMatch(int map,int mode,int bots,int difficulty,int team)
{
    if(m_page!=Play || map<0 || map>=m_maps.Count() || mode<0||mode>2||bots<0||bots>20||difficulty<0||difficulty>3||(team!=2&&team!=3))
    {SetMessage("请选择有效的地图和游戏选项");return;}
    if(!g_pFullFileSystem->FileExists(CFmtStr("maps/%s.bsp",GetMapName(map)),"GAME")) {SetMessage("地图文件不存在，请检查游戏资源");return;}
    m_map=map;m_mode=mode;m_bots=bots;m_difficulty=difficulty;m_team=team;
    if(!WriteConfig()) return;
    m_pendingMatch=true; SetMessage("");
    int type=mode==2?1:0, gameMode=mode==2?2:mode;
    engine->ClientCmd_Unrestricted(CFmtStr("disconnect; game_type %d; game_mode %d; bot_quota_mode normal; bot_quota %d; bot_difficulty %d; map %s",type,gameMode,bots,difficulty,GetMapName(map)));
}

const char *CMobileGameUI::GetSettingLabel(int i){return ValidSetting(i)?LocalizeText(kSettings[i].label):"";}
float CMobileGameUI::GetSettingMin(int i){return ValidSetting(i)?kSettings[i].min:0;}
float CMobileGameUI::GetSettingMax(int i){return ValidSetting(i)?kSettings[i].max:0;}
float CMobileGameUI::GetSettingStep(int i){return ValidSetting(i)?kSettings[i].step:1;}
float CMobileGameUI::GetSetting(int i){return ValidSetting(i)?m_draft[i]:0;}
void CMobileGameUI::SetSetting(int i,float value){if(m_page==Settings&&ValidSetting(i))m_draft[i]=CleanSetting(i,value);}
void CMobileGameUI::ReadLiveSettings()
{
    for(int i=0;i<SettingCount;++i)if(kSettings[i].cvar)
    {
        ConVarRef cv(kSettings[i].cvar); if(!cv.IsValid())continue;
        float value=cv.GetFloat(); if(i==3)value=2-value;
        if(i==4){int n=cv.GetInt();value=n>=16?4:n>=8?3:n>=4?2:n>=2?1:0;}
        if(i==kRenderScaleSetting)value*=100;
        m_settings[i]=CleanSetting(i,value);
    }
}
void CMobileGameUI::ApplySettings()
{
    cl_mobile_context.SetValue(m_settings[9]>.5f?1:0);
    for(int i=0;i<SettingCount;++i)if(kSettings[i].cvar)
    {
        ConVarRef cv(kSettings[i].cvar);if(!cv.IsValid())continue;
        float value=m_settings[i];if(i==3)value=2-value;if(i==4)value=float(1<<int(value));
        if(i==kRenderScaleSetting)value*=.01f;
        cv.SetValue(value);
    }
    // Source upscales the 3D viewport before drawing its native-resolution HUD.
    // Changing this setting must not resize the Surface or reset the D3D device.
    ConVarRef viewportUpscale("mat_viewportupscale");
    viewportUpscale.SetValue(1);
    ResetInput();
}
void CMobileGameUI::ReadConfig()
{
    KeyValues *kv=new KeyValues("MobileUI");
    if(kv->LoadFromFile(g_pFullFileSystem,m_configPath.String(),nullptr)&&kv->GetInt("version")==1)
    {
        if(KeyValues *settings=kv->FindKey("settings"))
        {
            for(int i=0;i<SettingCount;++i)m_settings[i]=CleanSetting(i,settings->GetFloat(kSettings[i].key,kSettings[i].defaultValue));
            // Older configs used one value for both views. Preserve that feel
            // until the player saves an independent scoped sensitivity.
            if(!settings->FindKey(kSettings[kScopedSensitivitySetting].key))
                m_settings[kScopedSensitivitySetting]=m_settings[kSensitivitySetting];
        }
        if(KeyValues *layout=kv->FindKey("layout"))
        {
            const int version=kv->GetInt("layout_version",1);
            bool oldDefault=version<3;
            for(int i=0;i<17&&oldDefault;++i)
            {
                const Button d=LegacyButton(i,version,m_touch.Aspect());
                if(KeyValues *b=layout->FindKey(i==8?"next":Definition(i).key))
                {
                    const float x=b->GetFloat("x",d.x),y=b->GetFloat("y",d.y),size=b->GetFloat("size",d.size),opacity=b->GetFloat("opacity",d.opacity);
                    oldDefault=Finite(x)&&Finite(y)&&Finite(size)&&Finite(opacity)&&fabsf(x-d.x)<.0001f&&fabsf(y-d.y)<.0001f
                        &&fabsf(size-d.size)<.0001f&&fabsf(opacity-d.opacity)<.0001f;
                }
            }
            if(!oldDefault)
            {
                // Keep authored positions for controls with stable semantic keys.
                // New equipment receives the new defaults; invnext is retired.
                for(int i=0;i<ControlCount;++i)if(KeyValues *b=layout->FindKey(Definition(i).key))
                {
                    Button &v=m_touch.buttons[i];
                    v.x=b->GetFloat("x",v.x);v.y=b->GetFloat("y",v.y);v.size=b->GetFloat("size",v.size);v.opacity=b->GetFloat("opacity",v.opacity);
                    m_touch.Normalize(i);
                }
                if(m_touch.Collapsed()) m_touch.Defaults();
            }
        }
        const char *map=kv->GetString("map","de_dust2");FOR_EACH_VEC(m_maps,i)if(!V_strcmp(m_maps[i],map))m_map=i;
        m_mode=Clamp(kv->GetInt("mode"),0,2);m_bots=Clamp(kv->GetInt("bots",8),0,20);m_difficulty=Clamp(kv->GetInt("difficulty",1),0,3);m_team=kv->GetInt("team",3)==2?2:3;
    }
    kv->deleteThis();for(int i=0;i<SettingCount;++i)m_draft[i]=m_settings[i];
}
bool CMobileGameUI::WriteConfig()
{
    KeyValues *kv=new KeyValues("MobileUI");kv->SetInt("version",1);kv->SetInt("layout_version",3);
    KeyValues *settings=kv->FindKey("settings",true);for(int i=0;i<SettingCount;++i)settings->SetFloat(kSettings[i].key,m_settings[i]);
    KeyValues *layout=kv->FindKey("layout",true);for(int i=0;i<ControlCount;++i)
    {KeyValues *b=layout->FindKey(Definition(i).key,true);const Button &v=m_touch.buttons[i];b->SetFloat("x",v.x);b->SetFloat("y",v.y);b->SetFloat("size",v.size);b->SetFloat("opacity",v.opacity);}
    kv->SetString("map",GetMapName(m_map));kv->SetInt("mode",m_mode);kv->SetInt("bots",m_bots);kv->SetInt("difficulty",m_difficulty);kv->SetInt("team",m_team);
    char directory[MAX_PATH];V_ExtractFilePath(m_configPath.String(),directory,sizeof(directory));g_pFullFileSystem->CreateDirHierarchy(directory,nullptr);
    CUtlString temporary(CFmtStr("%s.tmp",m_configPath.String()).Get());
    bool saved=kv->SaveToFile(g_pFullFileSystem,temporary.String(),nullptr);
    kv->deleteThis();
    if(saved)saved=std::rename(temporary.String(),m_configPath.String())==0;
    if(!saved){Warning("Mobile UI could not save %s\n",m_configPath.String());SetMessage("保存失败，请检查游戏配置目录的写入权限");}
    return saved;
}

void CMobileGameUI::SelectControl(int i){if(Editing()&&i>=0&&i<ControlCount){m_selectedControl=i;++m_revision;}}
const char *CMobileGameUI::GetControlLabel(int i){return i>=0&&i<ControlCount?LocalizeText(Definition(i).label):"";}
float CMobileGameUI::GetControlSize(){return m_touch.buttons[m_selectedControl].size;}
float CMobileGameUI::GetControlOpacity(){return m_touch.buttons[m_selectedControl].opacity;}
void CMobileGameUI::SetControlSize(float v){if(Editing()&&Finite(v)){m_touch.buttons[m_selectedControl].size=v;m_touch.Normalize(m_selectedControl);}}
void CMobileGameUI::SetControlOpacity(float v){if(Editing()&&Finite(v)){m_touch.buttons[m_selectedControl].opacity=v;m_touch.Normalize(m_selectedControl);}}

void CMobileGameUI::RefreshHudState()
{
    C_CSPlayer *player=C_CSPlayer::GetLocalCSPlayer();
    const bool alive=player&&player->IsAlive()&&!player->IsObserver()&&!player->IsPlayerGhost();
    const uint32_t token=player?player->GetRefEHandle().ToInt():0;
    const int team=player?player->GetTeamNumber():0;
    if(token!=m_playerToken||team!=m_playerTeam||alive!=m_wasAlive)
    {
        ResetInput();m_playerToken=token;m_playerTeam=team;m_wasAlive=alive;
    }
    m_playerState={};m_interactionState={};
    PlayerState &p=m_playerState;
    p.activeControl=-1;p.alive=alive;p.observing=player&&player->IsObserver();
    if(alive)
    {
        CWeaponCSBase *active=player->GetActiveCSWeapon();
        p.activeControl=WeaponControl(active);
        p.canMove=player->CanMove();p.canBuy=player->CanPlayerBuy(false);
        p.crouched=m_duckToggle||(player->GetFlags()&FL_DUCKING);p.walking=m_walkToggle||player->m_bIsWalking;
        p.scoped=player->m_bIsScoped;p.defusing=player->m_bIsDefusing;
        const bool busy=p.defusing||player->m_bIsGrabbingHostage||player->m_iBlockingUseActionInProgress||player->IsTaunting();
        p.canAttack=!busy&&!(player->GetFlags()&FL_FROZEN)&&!(CSGameRules()&&CSGameRules()->IsFreezePeriod());
        const int duration=player->GetProgressBarDuration();
        if(duration>0)p.progress=Clamp((gpGlobals->curtime-player->m_flProgressBarStartTime)/duration,0,1);
        for(int n=0;n<player->WeaponCount();++n)
        {
            CWeaponCSBase *weapon=dynamic_cast<CWeaponCSBase *>(player->GetWeapon(n));
            const int i=WeaponControl(weapon);
            if(i<0||weapon->GetPlayerOwner()!=player)continue;
            WeaponState &w=p.weapons[i];
            w.token=weapon->GetRefEHandle().ToInt();w.entity=weapon->entindex();w.selected=weapon==active;
            w.clip=weapon->UsesClipsForAmmo1()?weapon->Clip1():-1;
            w.reserve=weapon->GetReserveAmmoCount(AMMO_POSITION_PRIMARY);
            w.count=IsGrenadeControl(i)||i==Healthshot||i==BreachCharge||i==BumpMine?w.reserve:1;
            w.selectable=!busy&&weapon->CanBeSelected()&&(!active||active==weapon||active->CanHolster());
        }
        if(active&&p.activeControl>=0)
        {
            p.reloading=active->m_bInReload;
            p.canReload=p.canAttack&&active->UsesClipsForAmmo1()&&active->Clip1()<active->GetMaxClip1()
                &&active->GetReserveAmmoCount(AMMO_POSITION_PRIMARY)>0;
            p.canDrop=!busy&&(p.activeControl==Primary||p.activeControl==Pistol||p.activeControl==Taser||p.activeControl==Objective);
            if(CC4 *bomb=dynamic_cast<CC4 *>(active))p.planting=bomb->m_bStartedArming;
            if(active->HasZoom()||active->GetZoomLevels()>0)p.secondary=ScopeSecondary;
            else if(CWeaponCSBaseGun *gun=dynamic_cast<CWeaponCSBaseGun *>(active))
            {
                if(gun->WeaponHasBurst())p.secondary=BurstSecondary;
                else if(gun->HasDetachableSilencer())p.secondary=SilencerSecondary;
                else if(!V_strcmp(gun->GetClassname(),"weapon_revolver"))p.secondary=AlternateSecondary;
            }
            else if(IsGrenadeControl(p.activeControl))p.secondary=ThrowSecondary;
            else if(p.activeControl==Knife||p.activeControl==Fists||p.activeControl==Melee)p.secondary=StabSecondary;
            else if(p.activeControl==Shield)p.secondary=ShieldSecondary;
            else if(p.activeControl==Tablet)p.secondary=TabletSecondary;
        }
        ConVarRef anywhere("mp_plant_c4_anywhere");
        p.canPlant=p.weapons[Objective].entity&&p.canAttack&&(player->GetFlags()&FL_ONGROUND)
            &&(player->m_bInBombZone||(anywhere.IsValid()&&anywhere.GetBool()));
        C_BaseEntity *target=player->m_hMobileUseEntity.Get();
        const int kind=player->m_iMobileUseAction;
        if(target&&!target->IsDormant()&&kind>NoInteraction&&kind<InteractionKindCount)
        {
            m_interactionState.kind=InteractionKind(kind);m_interactionState.entity=target->entindex();
            m_interactionState.token=target->GetRefEHandle().ToInt();
            m_interactionState.progress=p.progress;
            if(C_PlantedC4 *bomb=dynamic_cast<C_PlantedC4 *>(target))
            {
                if(!bomb->IsBombActive()||bomb->m_bBombDefused||(bomb->m_hBombDefuser.Get()&&bomb->m_hBombDefuser.Get()!=player))m_interactionState={};
                else m_interactionState.progress=p.defusing?Clamp(1-bomb->GetDefuseProgress(),0,1):0;
            }
        }
    }
    m_hud=BuildHud(p,m_interactionState,Editing());
    m_touch.SetControls(m_hud);
}

void CMobileGameUI::DispatchActions()
{
    ControlMask frameMask=0;
    for(int i=0;i<ControlCount;++i)if(IsFrameAction(m_hud.controls[i].action))frameMask|=ControlBit(i);
    const ControlMask pressed=m_touch.TakePresses(frameMask);
    // A menu transition wins over all gameplay actions in the same event batch.
    if(pressed&ControlBit(Menu)){GameUI().ActivateGameUI();ResetInput();return;}
    if(pressed&ControlBit(Team)){GameUI().ActivateGameUI();SetMessage("选择下方队伍即可切换阵营");ResetInput();return;}
    for(int i=0;i<ControlCount;++i)
    {
        const ControlState &b=m_hud.controls[i];
        if(!(pressed&ControlBit(i))||!b.visible||!b.enabled)continue;
        switch(b.action)
        {
        case CrouchAction:m_duckToggle=!m_duckToggle;break;
        case WalkAction:m_walkToggle=!m_walkToggle;break;
        case EquipAction:
            if(OwnedWeapon(b.entity,b.token))
            {
                m_pendingWeaponEntity=b.entity;m_pendingWeaponToken=b.token;
                m_touch.CancelControl(Fire);m_touch.CancelControl(FireLeft);m_touch.CancelControl(Aim);
                m_touch.CancelControl(Objective);
            }
            break;
        case ScoreAction:m_showingScore=!m_showingScore;engine->ClientCmd_Unrestricted(m_showingScore?"+showscores":"-showscores");break;
        case BuyAction:engine->ClientCmd_Unrestricted("buymenu");ResetInput();return;
        case DropAction:
            if(CWeaponCSBase *weapon=OwnedWeapon(b.entity,b.token))
                if(weapon==C_CSPlayer::GetLocalCSPlayer()->GetActiveCSWeapon())engine->ClientCmd_Unrestricted("drop");
            break;
        case PreviousSpectatorAction:engine->ClientCmd_Unrestricted("spec_prev");break;
        case NextSpectatorAction:engine->ClientCmd_Unrestricted("spec_next");break;
        case SpectatorModeAction:engine->ClientCmd_Unrestricted("spec_mode");break;
        default:break;
        }
    }
}

void CMobileGameUI::UpdateControls()
{
    // Panorama rebuilds these panels back into the default flow after a resize
    // or when this window is hidden for team select. Cached rects still match,
    // so the buttons stay piled in the corner. Write the layout every frame.
    m_forceControlLayout=0;
    for(int i=0;i<ControlCount;++i)if(m_controls[i])
    {
        const ControlState &b=m_hud.controls[i];
        m_controls[i]->SetVisible(b.visible);
        if(!b.visible)continue;
        Rect r=m_touch.Bounds(i);IUIPanelStyle *style=m_controls[i]->AccessStyle();
        style->SetPositionWithoutTransition(Percent(r.x*100),Percent(r.y*100),Pixels(0));
        style->SetWidthWithoutTransition(Percent(r.w*100));
        style->SetHeightWithoutTransition(Percent(r.h*100));
        m_controlRects[i]=r;
        float opacity=m_touch.buttons[i].opacity*(Editing()?1:m_settings[8])*(b.enabled?1.f:.4f);
        style->SetOpacity(opacity);
        m_controlOpacity[i]=opacity;
        m_controls[i]->SetHasClass("pressed",m_touch.Held(i));m_controls[i]->SetHasClass("selected",Editing()&&i==m_selectedControl);
        m_controls[i]->SetHasClass("engaged",b.active);
        m_controls[i]->SetHasClass("equipped",b.equipped);
        m_controls[i]->SetHasClass("disabled",!b.enabled);
        CUtlString icon(CFmtStr(IsGameIcon(b.icon)?"file://{images}/icons/%s.svg":"file://{resources}/mobile/icons/%s.svg",b.icon).Get());
        const char *caption=b.label;
        CWeaponCSBase *weapon=IsInventoryControl(i)?OwnedWeapon(b.entity,b.token):nullptr;
        if(weapon)
        {
            if(m_weaponIconTokens[i]!=b.token)
            {
                m_weaponIconTokens[i]=b.token;m_weaponIconPaths[i]="";
                const char *name=weapon->GetItemDefinition()?weapon->GetDefinitionName():weapon->GetClassname();
                if(!V_strncmp(name,"weapon_",7))name+=7;
                if(g_pFullFileSystem->FileExists(CFmtStr("materials/panorama/images/icons/equipment/%s.svg",name),"GAME"))
                    m_weaponIconPaths[i]=CFmtStr("file://{images}/icons/equipment/%s.svg",name).Get();
            }
            if(m_weaponIconPaths[i].Length())icon=m_weaponIconPaths[i];
            if(i==Primary||i==Pistol||i==Knife||i==Melee)caption=weapon->GetPrintName();
        }
        if(m_icons[i]&&m_iconPaths[i]!=icon){m_icons[i]->SetImageJS(icon.String());m_iconPaths[i]=icon;}
        if(m_captions[i]&&m_captionTexts[i]!=caption){m_captions[i]->SetText(LocalizeText(caption));m_captionTexts[i]=caption;}
        CUtlString countText;
        if(b.clip>=0)countText=CFmtStr("%d / %d",b.clip,MAX(0,b.reserve)).Get();
        else if(b.count>0&&(IsGrenadeControl(i)||i==Healthshot||i==BreachCharge||i==BumpMine))countText=CFmtStr("×%d",b.count).Get();
        if(m_counts[i]&&m_countTexts[i]!=countText){m_counts[i]->SetText(countText.String());m_countTexts[i]=countText;}
        if(m_progressTracks[i])m_progressTracks[i]->SetVisible(b.progress>0);
        if(m_progressFills[i])m_progressFills[i]->AccessStyle()->SetWidthWithoutTransition(Percent(100*Clamp(b.progress,0,1)));
    }
    float x=30+m_touch.MoveX()*26,y=30-m_touch.MoveY()*26;
    if(m_stick&&(x!=m_stickX||y!=m_stickY))
    {
        m_stick->AccessStyle()->SetPositionWithoutTransition(Percent(x),Percent(y),Pixels(0));
        m_stickX=x;m_stickY=y;
    }
}
void CMobileGameUI::Frame()
{
    if(!s_input)return;
    static int layoutFrames=0;
    ++layoutFrames;
    if(layoutFrames==30)
    {
        IUIWindow *window=GetParentWindow();
        Msg("MOBILE_UI_LAYOUT: frame=%d window=%ux%u surface=%ux%u visible=%d lost=%d scale=%g page=%d state=%s\n",layoutFrames,window->GetWindowWidth(),window->GetWindowHeight(),window->GetSurfaceWidth(),window->GetSurfaceHeight(),window->BIsVisible(),window->BDeviceLost(),window->GetWindowScaleFactor(),int(m_page),CSGOGameUIStateName(GameUI().GetGameUIState()));
        Msg("MOBILE_UI_VIEWPORT: %dx%d safe=%g,%g,%g,%g\n",m_gameViewport.width,m_gameViewport.height,m_gameViewport.left,m_gameViewport.top,m_gameViewport.right,m_gameViewport.bottom);
    }
    UpdateVisibility();
    bool playing=Playing();if(playing!=m_playingLastFrame){ResetInput();m_playingLastFrame=playing;UpdateVisibility();}
    m_gameViewport=s_input->GetTouchViewport();MobileViewport view=m_gameViewport;
    const MobileViewport previousViewport=m_touch.Viewport();
    m_touch.SetViewport(view);
    const MobileViewport appliedViewport=m_touch.Viewport();
    if(IUIWindow *window=GetParentWindow())
    {
        const int ww=int(window->GetWindowWidth()), wh=int(window->GetWindowHeight());
        const int sw=int(window->GetSurfaceWidth()), sh=int(window->GetSurfaceHeight());
        if(ww!=m_layoutWindowW||wh!=m_layoutWindowH||sw!=m_layoutSurfaceW||sh!=m_layoutSurfaceH
            ||appliedViewport.width!=previousViewport.width||appliedViewport.height!=previousViewport.height
            ||appliedViewport.left!=previousViewport.left||appliedViewport.top!=previousViewport.top
            ||appliedViewport.right!=previousViewport.right||appliedViewport.bottom!=previousViewport.bottom)
        {
            m_layoutWindowW=ww; m_layoutWindowH=wh; m_layoutSurfaceW=sw; m_layoutSurfaceH=sh;
            m_forceControlLayout=45;
        }
    }
    RefreshHudState();
    MobileTouchEvent events[256];int count=s_input->ReadTouches(events,256);
    for(int i=0;i<count;++i)
    {
        const MobileTouchEvent &e=events[i];
        if(e.type==MOBILE_TOUCH_RESET){ResetInput();continue;}
        if(Editing())
        {
            if(e.type==MOBILE_TOUCH_DOWN)
            {
                CPanel2D *tools=FindChildInLayoutFile(m_editorToolsVisible?"EditorToolbar":"ShowEditorTools");
                float x=0,y=0;
                if(tools)tools->GetPositionWithinAncestor(this,&x,&y);
                const float px=e.x*GetActualLayoutWidth(),py=e.y*GetActualLayoutHeight();
                if(tools&&px>=x&&px<x+tools->GetActualLayoutWidth()&&py>=y&&py<y+tools->GetActualLayoutHeight())continue;
            }
            if(e.type==MOBILE_TOUCH_DOWN&&!m_editorFingerActive)
            {int control=m_touch.Hit(e.x,e.y);if(control>=0){m_selectedControl=control;++m_revision;m_editorFingerActive=true;m_editorFinger=e.finger;m_editorX=e.x;m_editorY=e.y;}}
            else if(m_editorFingerActive&&m_editorFinger==e.finger)
            {
                if(e.type==MOBILE_TOUCH_UP||e.type==MOBILE_TOUCH_CANCEL)m_editorFingerActive=false;
                else if(e.type==MOBILE_TOUCH_MOVE){m_touch.Drag(m_selectedControl,e.x-m_editorX,e.y-m_editorY);m_editorX=e.x;m_editorY=e.y;}
            }
        }
        else if(playing&&m_settings[9]>.5f)m_touch.Event(e);
    }
    if(playing)
    {
        DispatchActions();
    }
    UpdateControls();
}
void CMobileGameUI::ApplyGyro()
{
    if(!s_input) return;
    float yaw=0, pitch=0;
    if(!s_input->ReadGyro(yaw,pitch) || !Playing()) return;
    const int mode=int(m_settings[kGyroModeSetting]+.5f);
    C_CSPlayer *player=C_CSPlayer::GetLocalCSPlayer();
    if(!GyroViewActive(mode, player && player->IsAlive(), player && player->m_bIsScoped)) return;
    // ReadGyro is radians in view space. Sensitivity 1 keeps a 1:1 degree mapping.
    const float degrees=57.2957795f*m_settings[kGyroSensitivitySetting];
    QAngle angles; engine->GetViewAngles(angles);
    angles[YAW]+=yaw*degrees;
    const float pitchSign=m_settings[7]>.5f?-1.f:1.f;
    angles[PITCH]=Clamp(angles[PITCH]+pitch*degrees*pitchSign,-89.f,89.f);
    engine->SetViewAngles(angles);
}
void CMobileGameUI::MoveInput(CUserCmd *cmd)
{
    const bool touch=Playing()&&m_settings[9]>.5f;
    if(!touch){ResetInput();ApplyGyro();return;}
    float x,y;m_touch.TakeLook(x,y);
    C_CSPlayer *player=C_CSPlayer::GetLocalCSPlayer();
    const bool scoped=player&&player->IsAlive()&&player->m_bIsScoped;
    const float sensitivity=m_settings[scoped?kScopedSensitivitySetting:kSensitivitySetting];
    QAngle angles;engine->GetViewAngles(angles);
    angles[YAW]-=x*120*sensitivity;angles[PITCH]=Clamp(angles[PITCH]+y*120*sensitivity*(m_settings[7]>.5f?-1:1),-89,89);
    engine->SetViewAngles(angles);
    cmd->sidemove+=m_touch.MoveX()*450;cmd->forwardmove+=m_touch.MoveY()*450;
    ApplyGyro();
}
unsigned int CMobileGameUI::Buttons(CUserCmd *cmd)
{
    if(!Playing()||m_settings[9]<.5f)return 0;
    RefreshHudState();
    C_CSPlayer *player=C_CSPlayer::GetLocalCSPlayer();
    if(!player||!m_playerState.alive)return 0;
    ControlMask inputMask=0;
    for(int i=0;i<ControlCount;++i)if(!IsFrameAction(m_hud.controls[i].action))inputMask|=ControlBit(i);
    const ControlMask pressed=m_touch.TakePresses(inputMask),held=m_touch.HeldMask();
    unsigned int bits=0;
    bool selecting=false;
    if(CWeaponCSBase *weapon=OwnedWeapon(m_pendingWeaponEntity,m_pendingWeaponToken))
    {
        if(weapon->CanBeSelected()&&player->Weapon_CanSwitchTo(weapon))
        {
            cmd->weaponselect=weapon->entindex();cmd->weaponsubtype=weapon->GetSubType();selecting=true;
        }
    }
    m_pendingWeaponEntity=0;m_pendingWeaponToken=0;
    for(int i=0;i<ControlCount;++i)
    {
        const ControlState &b=m_hud.controls[i];
        if(!b.visible||!b.enabled||!((held|pressed)&ControlBit(i)))continue;
        switch(b.action)
        {
        case FireAction:if(!selecting)bits|=IN_ATTACK;break;
        case SecondaryHoldAction:if(!selecting)bits|=IN_ATTACK2;break;
        case SecondaryTapAction:if(!selecting&&(pressed&ControlBit(i)))bits|=IN_ATTACK2;break;
        case JumpAction:bits|=IN_JUMP;break;
        case ReloadAction:bits|=IN_RELOAD;break;
        case UseAction:bits|=IN_USE;break;
        case PlantAction:
            if(CWeaponCSBase *bomb=OwnedWeapon(b.entity,b.token))
            {
                if(bomb==player->GetActiveCSWeapon())bits|=IN_ATTACK;
                else if(!selecting&&player->Weapon_CanSwitchTo(bomb))
                {
                    cmd->weaponselect=bomb->entindex();cmd->weaponsubtype=bomb->GetSubType();selecting=true;
                }
                // Never fire the weapon that was equipped before holding C4.
                if(bomb!=player->GetActiveCSWeapon())bits&=~(IN_ATTACK|IN_ATTACK2);
            }
            break;
        default:break;
        }
    }
    if(m_duckToggle)bits|=IN_DUCK;
    if(m_walkToggle)bits|=IN_SPEED;
    if(m_touch.MoveY()>0)bits|=IN_FORWARD;else if(m_touch.MoveY()<0)bits|=IN_BACK;
    return bits;
}
void CMobileGameUI::Progress(float value,const char *status)
{
    value=Finite(value)?Clamp(value,0,1):0;
    if(CPanel2D *bar=FindChildInLayoutFile("LoadProgress"))bar->AccessStyle()->SetWidthWithoutTransition(Percent(value*100));
    SetTextChild(this,"LoadPercent",CFmtStr("%d%%",int(value*100)));
    SetTextChild(this,"LoadStage",status&&*status&&*status!='#'?status:"正在载入地图与游戏资源");
}
void CMobileGameUI::Failure(const char *reason){m_pendingMatch=false;SetMessage(reason&&*reason?reason:"地图加载失败，请检查资源后重试");}

void CMobileGameUI::PrintControls()
{
    Msg("MOBILE_HUD: alive=%d active=%d interaction=%d target=%d visible=%llx held=%llx\n",m_playerState.alive,
        m_playerState.activeControl,int(m_interactionState.kind),m_interactionState.entity,
        (unsigned long long)m_hud.Visible(),(unsigned long long)m_touch.HeldMask());
    for(int i=0;i<ControlCount;++i)if(m_hud.controls[i].visible)
    {
        const ControlState &b=m_hud.controls[i];const Rect r=m_touch.Bounds(i);
        Msg("MOBILE_CONTROL: %s action=%d entity=%d count=%d enabled=%d active=%d equipped=%d clip=%d reserve=%d icon=%s rect=%.4f,%.4f,%.4f,%.4f\n",
            Definition(i).key,int(b.action),b.entity,b.count,b.enabled,b.active,b.equipped,b.clip,b.reserve,
            m_iconPaths[i].String(),r.x,r.y,r.w,r.h);
    }
}

CON_COMMAND_F(mobileui_language,"Game language: auto, schinese or english; saved for the next launch.",FCVAR_NONE)
{
    if(!s_ui){Msg("Mobile UI is not active\n");return;}
    static const char *codes[]={"auto","schinese","english"};
    if(args.ArgC()>1)
    {
        int selected=-1;
        for(int i=0;i<3;++i)if(!V_strcmp(args[1],codes[i]))selected=i;
        if(selected<0){Msg("Usage: mobileui_language [auto|schinese|english]\n");return;}
        s_ui->SetGameLanguage(selected);
    }
    ConVarRef language("cl_language");
    Msg("[mobile language] preference=%s running=%s settings_label=%s\n",codes[s_ui->GetGameLanguage()],language.GetString(),s_ui->LocalizeText("设置"));
}

CON_COMMAND_F(mobileui_status,"Show mobile UI/input state",FCVAR_DEVELOPMENTONLY)
{
    if(s_ui)
    {
        Msg("MOBILE_UI: page=%d maps=%d state=%s\n",s_ui->GetPage(),s_ui->GetMapCount(),CSGOGameUIStateName(GameUI().GetGameUIState()));
        s_ui->PrintControls();
    }
}
