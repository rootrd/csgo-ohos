'use strict';
// CS:NO (09-29, 0.2.1, 老板): the video page keeps only what players need -- 亮度, 纵横比, 分辨率, 帧率上限 and the
// quality levels (阴影 / 模型贴图 / 特效 / 着色器 / FXAA / 纹理过滤).  Hidden: 显示器/电视, 显示模式 (全屏 mapped the
// touches wrong), 笔记本省电, 多核渲染 (always on), 多重采样抗锯齿 (画质预设 decides it; Turnip breaks with it),
// 垂直同步 (the 帧率上限 needs it on), 动态模糊.  CsnoQuality.fixBeforeStart keeps those values right in video.txt.

( function()
{
	var HIDE = [ 'ColorMode', 'DisplayModeEnum', 'PowerSavingsMode', 'MatQueueMode', 'AAMode', 'VSync', 'MotionBlur' ];
	var m_tries = 0;

	function _Row( p )
	{
		for ( var i = 0; p && i < 6; i++, p = p.GetParent() )
			if ( p.BHasClass( 'SettingsMenuDropdownContainer' ) )
				return p;
		return null;
	}

	function _Hide()
	{
		var root = $.GetContextPanel();
		if ( !root || !root.IsValid() )
			return;
		var missing = 0;
		HIDE.forEach( function( id )
		{
			var el = root.FindChildTraverse( id );
			var row = el ? _Row( el ) : null;
			if ( !row )
			{
				missing++;
				return;
			}
			row.visible = false;
			// the separator line under a hidden row goes too
			var par = row.GetParent();
			if ( par )
			{
				var idx = par.GetChildIndex( row );
				var next = idx >= 0 ? par.GetChild( idx + 1 ) : null;
				if ( next && next.BHasClass( 'horizontal-separator' ) )
					next.visible = false;
			}
		} );
		if ( missing && ( m_tries++ ) < 20 )
			$.Schedule( 0.25, _Hide );
	}

	// 老板 (09-29): 切换比例要即时生效 -- a new aspect ratio (the resolution list refills with that aspect's sizes,
	// the closest one selected) is applied by itself a moment later, as if 应用更改 were pressed.  Only user input
	// counts: the page's own dropdowns call SettingsMenuShared.VideoSettingsOnUserInputSubmit on a real change.
	var m_aspect = null;
	function _AspectId()
	{
		var dd = $.GetContextPanel().FindChildTraverse( 'AspectRatioEnum' );
		var sel = dd && dd.GetSelected ? dd.GetSelected() : null;
		return sel ? sel.id : null;
	}
	// settingsmenu_shared.js runs again whenever another settings page loads (a new SettingsMenuShared object), so
	// the wrapper is checked every second while this page exists
	var m_wrapper = null;
	function _HookAspect()
	{
		var root = $.GetContextPanel();
		if ( !root || !root.IsValid() )
			return;
		$.Schedule( 1.0, _HookAspect );
		if ( typeof SettingsMenuShared === 'undefined' || !SettingsMenuShared.VideoSettingsOnUserInputSubmit )
			return;
		if ( m_wrapper && SettingsMenuShared.VideoSettingsOnUserInputSubmit === m_wrapper )
			return;
		var orig = SettingsMenuShared.VideoSettingsOnUserInputSubmit.csnoOrig || SettingsMenuShared.VideoSettingsOnUserInputSubmit;
		if ( m_aspect === null )
			m_aspect = _AspectId();
		m_wrapper = function()
		{
			orig.apply( this, arguments );
			var now = _AspectId();
			if ( now && m_aspect !== null && now !== m_aspect )
			{
				m_aspect = now;
				$.Schedule( 0.4, function() { SettingsMenuShared.VideoSettingsApplyChanges(); } );
			}
			else if ( m_aspect === null )
				m_aspect = now;
		};
		m_wrapper.csnoOrig = orig;
		SettingsMenuShared.VideoSettingsOnUserInputSubmit = m_wrapper;
	}

	$.Schedule( 0.05, _Hide );
	$.Schedule( 0.6, _HookAspect );
} )();
