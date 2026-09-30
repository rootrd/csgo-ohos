#pragma once
#include "panorama/ui_root.h"
#include "gameui_interface.h"
#include "mobile_touch.h"

class CUserCmd;
class KeyValues;
namespace panorama { class CImagePanel; class CLabel; }
void MobileUIInitialize( CreateInterfaceFn factory );
bool MobileUIEnabled();
void MobileUIFrame();
void MobileUIMove( CUserCmd *cmd );
unsigned int MobileUIButtons( CUserCmd *cmd );
void MobileUILoadProgress( float progress, const char *status );
void MobileUILoadFailure( const char *reason );

// In-game Panorama pointer menus (team select, buy menu) borrow UI clicks.
// Each open panel adds one; the count returns to game touch at zero.
void MobileUIAdjustPointerMenu( int delta );

class CMobileGameUI : public CUI_Root, public ICSGOGameUIStateListener
{
    DECLARE_PANEL2D( CMobileGameUI, CUI_Root );
public:
    enum Page { Home, Play, Settings, Layout, Loading, ConfirmQuit, ConfirmDisconnect };
    enum { SettingCount=15 };
    CMobileGameUI( panorama::CPanel2D *parent, const char *id );
    ~CMobileGameUI();
    void SetupJavascriptObjectTemplate() override;
    void OnCSGOGameUIStateChange( CSGOGameUIState_t oldState, CSGOGameUIState_t state ) override;
    void OnMapLoadStarted( const char *level, bool connecting ) override;
    void OnMapLoadFinished() override;
    void OnGameLoopDeactivated() override;
    void Frame();
    void MoveInput( CUserCmd *cmd );
    unsigned int Buttons( CUserCmd *cmd );
    void Progress( float value, const char *status );
    void Failure( const char *reason );
    void AdjustPointerMenu( int delta );
    void PrintControls();

    void Action( const char *action );
    int GetPage() { return int(m_page); }
    int GetRevision() { return m_revision; }
    bool IsInMatch();
    const char *GetMessage() { return m_message.String(); }
    int GetMapCount() { return m_maps.Count(); }
    const char *GetMapName( int index );
    int GetSelectedMap() { return m_map; }
    int GetMode() { return m_mode; }
    int GetBots() { return m_bots; }
    int GetDifficulty() { return m_difficulty; }
    int GetTeam() { return m_team; }
    void StartMatch( int map, int mode, int bots, int difficulty, int team );
    int GetSettingCount() { return SettingCount; }
    const char *GetSettingLabel( int i );
    float GetSettingMin( int i );
    float GetSettingMax( int i );
    float GetSettingStep( int i );
    float GetSetting( int i );
    void SetSetting( int i, float value );
    int GetGameLanguage() { return m_gameLanguage; }
    void SetGameLanguage( int language );
    const char *LocalizeText( const char *text );
    int GetSelectedControl() { return m_selectedControl; }
    void SelectControl( int i );
    const char *GetControlLabel( int i );
    float GetControlSize();
    float GetControlOpacity();
    void SetControlSize( float value );
    void SetControlOpacity( float value );

private:
    void SetPage( Page page );
    void UpdateVisibility();
    void ResetInput();
    void ScanMaps();
    void ReadConfig();
    bool WriteConfig();
    void ApplySettings();
    void ReadLiveSettings();
    void ReadGameLanguage();
    void TranslateLabels( panorama::CPanel2D *panel );
    void ApplyGyro();
    void UpdateControls();
    void RefreshHudState();
    void DispatchActions();
    void SetMessage( const char *message );
    bool Playing();
    bool Editing() { return m_page==Layout; }
    mobile::TouchControls m_touch;
    mobile::Button m_savedLayout[mobile::ControlCount];
    panorama::CPanel2D *m_controls[mobile::ControlCount];
    panorama::CImagePanel *m_icons[mobile::ControlCount];
    panorama::CLabel *m_captions[mobile::ControlCount], *m_counts[mobile::ControlCount];
    panorama::CPanel2D *m_progressTracks[mobile::ControlCount], *m_progressFills[mobile::ControlCount];
    CUtlString m_iconPaths[mobile::ControlCount], m_captionTexts[mobile::ControlCount], m_countTexts[mobile::ControlCount];
    CUtlString m_weaponIconPaths[mobile::ControlCount];
    uint32_t m_weaponIconTokens[mobile::ControlCount];
    mobile::PlayerState m_playerState;
    mobile::InteractionState m_interactionState;
    mobile::HudState m_hud;
    uint32_t m_playerToken, m_pendingWeaponToken;
    int m_playerTeam, m_pendingWeaponEntity;
    bool m_duckToggle, m_walkToggle, m_wasAlive;
    panorama::CPanel2D *m_stick;
    mobile::Rect m_controlRects[mobile::ControlCount];
    float m_controlOpacity[mobile::ControlCount], m_stickX, m_stickY;
    Page m_page;
    int m_revision;
    float m_settings[SettingCount], m_draft[SettingCount];
    CUtlString m_message, m_configPath, m_loadingMap;
    CUtlString m_languagePath;
    KeyValues *m_englishStrings;
    int m_gameLanguage;
    bool m_englishUI;
    CUtlVector<CUtlString> m_maps;
    int m_map, m_mode, m_bots, m_difficulty, m_team, m_selectedControl;
    bool m_pendingMatch, m_showingScore, m_editorFingerActive, m_playingLastFrame;
    int m_pointerMenus;
    bool m_editorToolsVisible;
    int64_t m_editorFinger;
    float m_editorX, m_editorY;
    uint64 m_denyInput;
    MobileViewport m_gameViewport;
    int m_layoutWindowW, m_layoutWindowH, m_layoutSurfaceW, m_layoutSurfaceH;
    int m_forceControlLayout;
};
