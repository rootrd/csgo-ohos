'use strict';
// CS:NO (09-29, 0.2.3, 老板: 自定义准心): settings tab 准星 (layout/settings/settings_csno_crosshair.xml).
// The controls are the game's own (CSGOSettingsSlider / CSGOSettingsEnumDropDown write the convars; the settings
// menu saves them with host_writeconfig when it closes).  This draws the preview: the rectangles
// CWeaponCSBase::DrawCrosshair (game/shared/cstrike15/weapon_csbase.cpp) draws for a player standing still --
//   bar = YRES( cl_crosshairsize ), thickness = max( 1, YRES( cl_crosshairthickness ) ), YRES(x) = x * screen h / 480
//   distance: styles 0/1 4 * h / 1200 + gap, the others 4 + gap (pixels, not scaled)
//   outline: black, cl_crosshair_outlinethickness pixels around every piece; T style: no top bar
// in engine pixels, shown at their real size (panorama px = engine px / ui scale).

var CsnoCrosshair = ( function()
{
	var COLORS = [ [ 250, 50, 50 ], [ 50, 250, 50 ], [ 250, 250, 50 ], [ 50, 50, 250 ], [ 50, 250, 250 ] ];
	var PRESETS = {
		'default': { cl_crosshairstyle: 2, cl_crosshairsize: 5, cl_crosshairthickness: 0.5, cl_crosshairgap: 1,
			cl_crosshairdot: 0, cl_crosshair_t: 0, cl_crosshair_drawoutline: 1, cl_crosshair_outlinethickness: 1,
			cl_crosshaircolor: 1, cl_crosshairusealpha: 1, cl_crosshairalpha: 200, cl_crosshairgap_useweaponvalue: 0 },
		'small': { cl_crosshairstyle: 4, cl_crosshairsize: 2, cl_crosshairthickness: 1, cl_crosshairgap: -2,
			cl_crosshairdot: 0, cl_crosshair_t: 0, cl_crosshair_drawoutline: 1, cl_crosshair_outlinethickness: 1,
			cl_crosshaircolor: 1, cl_crosshairusealpha: 1, cl_crosshairalpha: 255, cl_crosshairgap_useweaponvalue: 0 },
		'dot': { cl_crosshairstyle: 4, cl_crosshairsize: 0, cl_crosshairthickness: 1.5, cl_crosshairgap: -2,
			cl_crosshairdot: 1, cl_crosshair_t: 0, cl_crosshair_drawoutline: 1, cl_crosshair_outlinethickness: 1,
			cl_crosshaircolor: 4, cl_crosshairusealpha: 1, cl_crosshairalpha: 255, cl_crosshairgap_useweaponvalue: 0 }
	};
	var m_last = '';
	var m_running = false;

	function _Root() { return $.GetContextPanel(); }
	function _F( name, def ) { var v = parseFloat( GameInterfaceAPI.GetSettingString( name ) ); return isNaN( v ) ? def : v; }

	function _Init()
	{
		if ( m_running )
			return;
		m_running = true;
		_Tick();
	}

	function _Tick()
	{
		var root = _Root();
		if ( !root || !root.IsValid() )
		{
			m_running = false;
			return;
		}
		_Draw();      // keyed on the values: a no-op unless something changed
		$.Schedule( 0.1, _Tick );
	}

	function _Rect( layer, x0, y0, x1, y1, rgba, s )
	{
		if ( x1 <= x0 || y1 <= y0 )
			return;
		var p = $.CreatePanel( 'Panel', layer, '' );
		p.hittest = false;
		p.style.position = ( x0 / s ) + 'px ' + ( y0 / s ) + 'px 0px';
		p.style.width = ( ( x1 - x0 ) / s ) + 'px';
		p.style.height = ( ( y1 - y0 ) / s ) + 'px';
		p.style.backgroundColor = rgba;
	}

	function _Draw()
	{
		var layer = _Root().FindChildTraverse( 'CsnoXhLayer' );
		if ( !layer )
			return;
		var st = Math.round( _F( 'cl_crosshairstyle', 2 ) );
		var size = _F( 'cl_crosshairsize', 5 ), thick = _F( 'cl_crosshairthickness', 0.5 ), gap = _F( 'cl_crosshairgap', 1 );
		var dot = _F( 'cl_crosshairdot', 0 ) !== 0, tee = _F( 'cl_crosshair_t', 0 ) !== 0;
		var outline = _F( 'cl_crosshair_drawoutline', 1 ) !== 0, olt = _F( 'cl_crosshair_outlinethickness', 1 );
		var ci = Math.round( _F( 'cl_crosshaircolor', 1 ) );
		var useAlpha = _F( 'cl_crosshairusealpha', 1 ) !== 0;
		var alpha = useAlpha ? Math.max( 0, Math.min( 255, Math.round( _F( 'cl_crosshairalpha', 200 ) ) ) ) : 200;
		var c = ci === 5 ? [ _F( 'cl_crosshaircolor_r', 50 ), _F( 'cl_crosshaircolor_g', 250 ), _F( 'cl_crosshaircolor_b', 50 ) ]
			: ( COLORS[ ci ] || COLORS[ 1 ] );

		var s = layer.actualuiscale_y || 1;             // engine px per panorama px
		var w = layer.actuallayoutwidth || 0, h = layer.actuallayoutheight || 0;   // engine px
		var key = [ st, size, thick, gap, dot, tee, outline, olt, ci, c.join( ',' ), alpha, w, h, s ].join( '|' );
		if ( key === m_last )
			return;
		m_last = key;
		layer.RemoveAndDeleteChildren();
		if ( w <= 0 || h <= 0 )
			return;

		var H = 1080 * s;                               // the engine's screen height
		var bar = Math.round( size * H / 480 );
		var th = Math.max( 1, Math.round( thick * H / 480 ) );
		var dist = ( st === 0 || st === 1 ) ? Math.round( 4 * H / 1200 + gap ) : Math.round( 4 + gap );
		var cx = Math.round( w / 2 ), cy = Math.round( h / 2 );
		var half = Math.floor( th / 2 );

		var rects = [];
		var innerL = cx - dist - half, innerR = innerL + 2 * dist + th;
		var y0 = cy - half, y1 = y0 + th;
		rects.push( [ innerL - bar, y0, innerL, y1 ] );
		rects.push( [ innerR, y0, innerR + bar, y1 ] );
		var innerT = cy - dist - half, innerB = innerT + 2 * dist + th;
		var x0 = cx - half, x1 = x0 + th;
		if ( !tee )
			rects.push( [ x0, innerT - bar, x1, innerT ] );
		rects.push( [ x0, innerB, x1, innerB + bar ] );
		if ( dot )
			rects.push( [ x0, cy - half, x1, cy - half + th ] );

		var a = ( alpha / 255 ).toFixed( 3 );
		for ( var i = 0; i < rects.length; i++ )
		{
			var r = rects[ i ];
			if ( r[ 2 ] <= r[ 0 ] || r[ 3 ] <= r[ 1 ] )
				continue;
			if ( outline )
				_Rect( layer, r[ 0 ] - olt, r[ 1 ] - olt, r[ 2 ] + olt, r[ 3 ] + olt, 'rgba(0,0,0,' + a + ')', s );
			_Rect( layer, r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], 'rgba(' + Math.round( c[ 0 ] ) + ',' + Math.round( c[ 1 ] ) + ','
				+ Math.round( c[ 2 ] ) + ',' + a + ')', s );
		}
	}

	// settingsmenu_shared.js _RefreshControlsRecursive (not exported): the settings controls re-read their convar in OnShow
	function _Refresh( panel )
	{
		if ( !panel )
			return;
		if ( panel.OnShow !== undefined )
			panel.OnShow();
		if ( panel.GetChildCount === undefined )
			return;
		for ( var i = 0; i < panel.GetChildCount(); i++ )
			_Refresh( panel.GetChild( i ) );
	}

	function _Preset( id )
	{
		var p = PRESETS[ id ];
		if ( !p )
			return;
		for ( var k in p )
			GameInterfaceAPI.SetSettingString( k, String( p[ k ] ) );
		_Refresh( _Root() );
		GameInterfaceAPI.ConsoleCommand( 'host_writeconfig' );
		m_last = '';
		_Draw();
	}

	return { Init: _Init, Preset: _Preset };
} )();
