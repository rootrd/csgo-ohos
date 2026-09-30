'use strict';
// CS:NO (09-28, 0.1.5): the full-screen touch layout editor (layout/popups/popup_csno_touch_editor.xml).
// Layout from the app (CsnoBridge.java touch_get -> TouchControls.layoutJson):
//   { w, h, u (short edge / h), opacity, dynamic, stick: {x, y, r}, buttons: [{id, label, def, x, y, r, a, hidden, style}] }
//   x, y = centre as a fraction of the screen, r = radius as a fraction of the short edge.
// Saved piece by piece (a console line holds 512 characters): touch_opts / touch_stick / touch_btn ..., touch_commit.

var CsnoTouchEditor = ( function()
{
	var GRID_COLS = 32, GRID_ROWS = 16;
	var STICK = '__stick';
	var m_layout = null;
	var m_items = [];           // { id, label, x, y, r, a, hidden, style, stick }
	var m_sel = null;
	var m_side = 'center';     // the middle of the screen has no buttons; 面板换边 cycles center -> left -> right
	var m_updating = false;

	function _El( id ) { return $.GetContextPanel().FindChildTraverse( id ); }

	function _Init()
	{
		_BuildGrid();
		_WireSide();
		_Load();
	}

	function _Load()
	{
		CsnoBridge.CallJson( 'touch_get', undefined, function( l )
		{
			if ( !l || !l.buttons )
			{
				_El( 'CsnoSelTitle' ).text = '读取键位失败（请更新 CS:NO）';
				return;
			}
			m_layout = l;
			m_items = [];
			m_items.push( { id: STICK, label: '摇杆', x: l.stick.x, y: l.stick.y, r: l.stick.r, a: 1, hidden: false, style: -1, stick: true } );
			for ( var i = 0; i < l.buttons.length; i++ )
			{
				var b = l.buttons[ i ];
				m_items.push( { id: b.id, label: b.label, def: b.def, x: b.x, y: b.y, r: b.r, a: b.a, hidden: !!b.hidden, style: b.style, stick: false } );
			}
			_RenderAll();
			_Select( null );
		} );
	}

	function _BuildGrid()
	{
		var grid = _El( 'CsnoGrid' );
		grid.RemoveAndDeleteChildren();
		for ( var r = 0; r < GRID_ROWS; r++ )
		{
			var row = $.CreatePanel( 'Panel', grid, '' );
			row.AddClass( 'csno-grid-row' );
			for ( var c = 0; c < GRID_COLS; c++ )
			{
				var cell = $.CreatePanel( 'Panel', row, '' );
				cell.AddClass( 'csno-grid-cell' );
				( function( cc, rr )
				{
					cell.SetPanelEvent( 'onactivate', function() { _MoveTo( ( cc + 0.5 ) / GRID_COLS, ( rr + 0.5 ) / GRID_ROWS ); } );
				} )( c, r );
			}
		}
	}

	function _Aspect()
	{
		return m_layout && m_layout.h > 0 ? m_layout.w / m_layout.h : 16 / 9;
	}

	function _RenderAll()
	{
		var layer = _El( 'CsnoCircles' );
		layer.RemoveAndDeleteChildren();
		for ( var i = 0; i < m_items.length; i++ )
			_Render( m_items[ i ], layer );
	}

	function _Render( it, layer )
	{
		layer = layer || _El( 'CsnoCircles' );
		var p = layer.FindChild( 'csno_' + it.id );
		if ( !p )
		{
			p = $.CreatePanel( 'Panel', layer, 'csno_' + it.id );
			p.AddClass( 'csno-circle' );
			if ( it.stick )
				p.AddClass( 'csno-circle--stick' );
			else if ( it.style === 1 )
				p.AddClass( 'csno-circle--fire' );
			var lab = $.CreatePanel( 'Label', p, '' );
			lab.AddClass( 'csno-btn-text' );
			lab.hittest = false;
			p.SetPanelEvent( 'onactivate', function() { _Select( it ); } );
		}
		var u = m_layout ? m_layout.u : 1;
		var rh = it.r * u * 100;                 // radius, % of the screen height
		var rw = rh / _Aspect();                 // ... of the width
		p.style.width = ( 2 * rw ) + '%';
		p.style.height = ( 2 * rh ) + '%';
		p.style.position = ( it.x * 100 - rw ) + '% ' + ( it.y * 100 - rh ) + '% 0px';
		p.style.opacity = it.stick ? '1' : String( Math.max( 0.35, it.a ) );
		p.SetHasClass( 'csno-circle--hidden', it.hidden );
		p.SetHasClass( 'csno-circle--selected', m_sel === it );
		p.GetChild( 0 ).text = it.label;
	}

	function _Select( it )
	{
		var prev = m_sel;
		m_sel = it;
		if ( prev )
			_Render( prev );
		m_updating = true;
		if ( !it )
		{
			_El( 'CsnoSelTitle' ).text = '点一个按钮来编辑';
		}
		else
		{
			_Render( it );
			_El( 'CsnoSelTitle' ).text = '正在编辑：' + it.label;
			_El( 'CsnoName' ).text = it.label;
			_El( 'CsnoName' ).enabled = !it.stick;
			var size = _El( 'CsnoSize' );
			size.min = it.stick ? 60 : 25;
			size.max = it.stick ? 350 : 250;
			size.value = Math.round( it.r * 1000 );
			_El( 'CsnoSizeValue' ).text = Math.round( it.r * 1000 ) / 10 + '';
			_El( 'CsnoAlpha' ).value = Math.round( it.a * 100 );
			_El( 'CsnoAlpha' ).enabled = !it.stick;
			_El( 'CsnoAlphaValue' ).text = Math.round( it.a * 100 ) + '%';
			_El( 'CsnoHide' ).enabled = !it.stick;
			_El( 'CsnoHideLabel' ).text = it.hidden ? '显示这个按钮' : '隐藏这个按钮';
		}
		m_updating = false;
	}

	function _WireSide()
	{
		var size = _El( 'CsnoSize' );
		size.SetPanelEvent( 'onvaluechanged', function()
		{
			if ( m_updating || !m_sel )
				return;
			m_sel.r = size.value / 1000;
			_El( 'CsnoSizeValue' ).text = Math.round( size.value ) / 10 + '';
			_Render( m_sel );
		} );
		var alpha = _El( 'CsnoAlpha' );
		alpha.SetPanelEvent( 'onvaluechanged', function()
		{
			if ( m_updating || !m_sel || m_sel.stick )
				return;
			m_sel.a = alpha.value / 100;
			_El( 'CsnoAlphaValue' ).text = Math.round( alpha.value ) + '%';
			_Render( m_sel );
		} );
		var name = _El( 'CsnoName' );
		name.SetPanelEvent( 'ontextentrychange', function()
		{
			if ( m_updating || !m_sel || m_sel.stick )
				return;
			var t = name.text.replace( /\s+/g, '' );
			if ( t.length > 0 )
			{
				m_sel.label = t.substring( 0, 6 );
				_El( 'CsnoSelTitle' ).text = '正在编辑：' + m_sel.label;
				_Render( m_sel );
			}
		} );
	}

	function _Clamp( v, lo, hi ) { return Math.max( lo, Math.min( hi, v ) ); }

	function _MoveTo( x, y )
	{
		if ( !m_sel )
			return;
		m_sel.x = _Clamp( x, 0, 1 );
		m_sel.y = _Clamp( y, 0, 1 );
		_Render( m_sel );
	}

	function _Nudge( dx, dy )
	{
		if ( m_sel )
			_MoveTo( m_sel.x + dx * 0.005, m_sel.y + dy * 0.008 );
	}

	function _ToggleHidden()
	{
		if ( !m_sel || m_sel.stick )
			return;
		m_sel.hidden = !m_sel.hidden;
		_El( 'CsnoHideLabel' ).text = m_sel.hidden ? '显示这个按钮' : '隐藏这个按钮';
		_Render( m_sel );
	}

	function _F( v ) { return ( Math.round( v * 10000 ) / 10000 ).toString(); }

	function _Save()
	{
		if ( !m_layout )
			return _Close();
		CsnoBridge.Call( 'touch_opts', _F( m_layout.opacity || 1 ) + ' ' + ( m_layout.dynamic ? '1' : '0' ) + ' '
			+ ( m_layout.hide_all ? '1' : '0' ), null );
		for ( var i = 0; i < m_items.length; i++ )
		{
			var it = m_items[ i ];
			if ( it.stick )
				CsnoBridge.Call( 'touch_stick', _F( it.x ) + ' ' + _F( it.y ) + ' ' + _F( it.r ), null );
			else
				CsnoBridge.Call( 'touch_btn', it.id + ' ' + _F( it.x ) + ' ' + _F( it.y ) + ' ' + _F( it.r ) + ' ' + _F( it.a ) + ' '
					+ ( it.hidden ? '1' : '0' ) + ' ' + it.label, null );
		}
		CsnoBridge.Call( 'touch_commit', undefined, function()
		{
			_Close();
		} );
	}

	function _Defaults()
	{
		UiToolkitAPI.ShowGenericPopupTwoOptions( '全部恢复默认', '所有按钮都回到默认的位置、大小、透明度和名字。', '',
			'恢复默认', function() { CsnoBridge.Call( 'touch_reset', undefined, function() { _Load(); } ); },
			'取消', function() {} );
	}

	function _MoveSide()
	{
		var side = _El( 'CsnoSide' );
		m_side = m_side === 'center' ? 'left' : ( m_side === 'left' ? 'right' : 'center' );
		side.SetHasClass( 'csno-side--left', m_side === 'left' );
		side.SetHasClass( 'csno-side--right', m_side === 'right' );
	}

	function _Close()
	{
		$.DispatchEvent( 'UIPopupButtonClicked', '' );
	}

	function _Cancel()
	{
		_Close();
	}

	return {
		Init: _Init,
		Nudge: _Nudge,
		ToggleHidden: _ToggleHidden,
		Save: _Save,
		Cancel: _Cancel,
		Defaults: _Defaults,
		MoveSide: _MoveSide
	};
} )();
