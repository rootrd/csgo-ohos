'use strict';
// CS:NO (09-28, 0.1.5): the in-game touch controls, drawn by Panorama in the HUD (layout/hud/hud.xml kpatch).  老板:
// 键位也变成 panorama 渲染.  The app's TouchControls still does all the input; this only draws:
//   every 50 ms  "csno_java h<n> touch_state" -> "rev|active|stickX,stickY,stickR,knobX,knobY,held|pressed"
//   rev changed -> "csno_java h<n> touch_get" (the layout JSON the key-layout editor uses)
// Replies come back in the convar csno_java_reply_hud (client csno_bridge.cpp), apart from the main-menu pages'
// csno_java_reply.  While these polls arrive, TouchControls stops drawing; when they stop it draws again.

var CsnoTouchHud = ( function()
{
	var POLL = 0.05;
	var m_root = null;
	var m_seq = 0;
	var m_rev = -1;
	var m_fetching = false;
	var m_layout = null;
	var m_btns = [];            // { panel, style, down }
	var m_stick = null, m_knob = null;
	var m_last = '';

	// ---- a tiny bridge of its own (the "h" channel) ----
	function _Call( verb, cb )
	{
		m_seq = ( m_seq % 100000 ) + 1;
		var seq = 'h' + m_seq;
		GameInterfaceAPI.ConsoleCommand( 'csno_java ' + seq + ' ' + verb );
		var tries = 0;
		var poll = function()
		{
			var r = GameInterfaceAPI.GetSettingString( 'csno_java_reply_hud' ) || '';
			var bar = r.indexOf( '|' );
			if ( bar > 0 && r.substring( 0, bar ) === seq )
			{
				cb( r.substring( bar + 1 ) );
				return;
			}
			if ( ++tries > 25 )     // 0.5 s: no app / old client -- the app keeps drawing the controls itself
			{
				cb( null );
				return;
			}
			$.Schedule( 0.02, poll );
		};
		$.Schedule( 0.0, poll );
	}

	function _Init()
	{
		m_root = $.GetContextPanel().FindChildTraverse( 'CsnoTouchHud' );
		if ( !m_root )
			return;
		m_root.SetHasClass( 'csno-hud-off', true );
		_Tick();
	}

	function _Tick()
	{
		if ( !m_root || !m_root.IsValid() )
			return;     // the HUD was rebuilt (map change): the new copy of this script polls now
		_Call( 'touch_state', function( t )
		{
			if ( !m_root.IsValid() )
				return;
			if ( t && t.indexOf( '|' ) > 0 )
				_Apply( t );
			$.Schedule( t ? POLL : 1.0, _Tick );      // no answer: back off, try again in a second
		} );
	}

	function _Place( p, x, y, rh, aspect )
	{
		var rw = rh / aspect;
		p.style.width = ( 2 * rw ) + '%';
		p.style.height = ( 2 * rh ) + '%';
		p.style.position = ( x * 100 - rw ) + '% ' + ( y * 100 - rh ) + '% 0px';
	}

	function _Build( l )
	{
		m_layout = l;
		m_root.RemoveAndDeleteChildren();
		m_btns = [];
		var aspect = l.h > 0 ? l.w / l.h : 16 / 9;
		var u = l.u || 1;                         // short edge / height
		m_stick = $.CreatePanel( 'Panel', m_root, 'CsnoHudStick' );
		m_stick.AddClass( 'csno-hud-stick' );
		m_stick.hittest = false;
		m_knob = $.CreatePanel( 'Panel', m_root, 'CsnoHudKnob' );
		m_knob.AddClass( 'csno-hud-knob' );
		m_knob.hittest = false;
		for ( var i = 0; i < l.buttons.length; i++ )
		{
			var b = l.buttons[ i ];
			var p = $.CreatePanel( 'Panel', m_root, 'CsnoHudBtn' + i );
			p.AddClass( 'csno-hud-btn' );
			if ( b.style === 1 )
				p.AddClass( 'csno-hud-fire' );
			else if ( b.style === 2 )
				p.AddClass( 'csno-hud-slot' );
			p.hittest = false;
			var lab = $.CreatePanel( 'Label', p, '' );
			lab.AddClass( 'csno-hud-label' );
			lab.hittest = false;
			lab.text = b.label;
			_Place( p, b.x, b.y, b.r * u * 100, aspect );
			var o = ( l.opacity || 1 ) * ( b.a === undefined ? 1 : b.a );
			p.style.opacity = String( o );
			lab.style.opacity = String( o > 0 ? Math.max( 0.35, o ) / o : 1 );
			p.SetHasClass( 'csno-hud-hidden', !!b.hidden );
			m_btns.push( { panel: p, down: false } );
		}
		m_last = '';
	}

	function _Apply( t )
	{
		var parts = t.split( '|' );
		if ( parts.length < 4 )
			return;
		var rev = parseInt( parts[ 0 ], 10 );
		var active = parts[ 1 ] === '1';
		if ( rev !== m_rev && !m_fetching )
		{
			m_fetching = true;
			_Call( 'touch_get', function( j )
			{
				m_fetching = false;
				var l = null;
				try { l = j ? JSON.parse( j ) : null; } catch ( e ) { l = null; }
				if ( l && l.buttons && l.stick )
				{
					m_rev = rev;
					_Build( l );
				}
			} );
		}
		m_root.SetHasClass( 'csno-hud-off', !active || !m_layout );
		if ( !m_layout || !active || t === m_last )
			return;
		m_last = t;

		var aspect = m_layout.h > 0 ? m_layout.w / m_layout.h : 16 / 9;
		var s = parts[ 2 ].split( ',' );
		var held = s[ 5 ] === '1';
		var so = ( m_layout.opacity || 1 ) * ( m_layout.dynamic && !held ? 0.7 : 1 );
		var sr = parseFloat( s[ 2 ] ) * 100;
		_Place( m_stick, parseFloat( s[ 0 ] ), parseFloat( s[ 1 ] ), sr, aspect );
		m_stick.style.opacity = String( so );
		_Place( m_knob, parseFloat( s[ 3 ] ), parseFloat( s[ 4 ] ), sr * 0.38, aspect );
		m_knob.style.opacity = String( so );
		m_knob.SetHasClass( 'csno-hud-down', held );

		var pressed = parts[ 3 ];
		for ( var i = 0; i < m_btns.length && i < pressed.length; i++ )
		{
			var down = pressed.charAt( i ) === '1';
			if ( down !== m_btns[ i ].down )
			{
				m_btns[ i ].down = down;
				m_btns[ i ].panel.SetHasClass( 'csno-hud-down', down );
			}
		}
	}

	return { Init: _Init };
} )();

CsnoTouchHud.Init();
