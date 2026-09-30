'use strict';
// CS:NO (09-28, 0.1.5): settings tab 触控键位 (layout/settings/settings_csno_touch.xml).

var CsnoTouchSettings = ( function()
{
	var m_layout = null;
	var m_saveTimer = null;
	var m_loading = false;

	function _El( id ) { return $.GetContextPanel().FindChildTraverse( id ); }

	function _Init()
	{
		m_loading = true;
		CsnoBridge.CallJson( 'touch_get', undefined, function( l )
		{
			m_layout = l;
			if ( l )
			{
				var dyn = _El( 'CsnoDynamicStick' );
				dyn.checked = !!l.dynamic;
				_El( 'CsnoDynamicLabel' ).text = l.dynamic ? '开' : '关';
				_El( 'CsnoHideAll' ).checked = !!l.hide_all;
				_El( 'CsnoHideAllLabel' ).text = l.hide_all ? '开' : '关';
				var sl = _El( 'CsnoOpacity' );
				sl.value = Math.round( ( l.opacity || 1 ) * 100 );
				_El( 'CsnoOpacityValue' ).text = Math.round( sl.value ) + '%';
			}
			m_loading = false;
		} );
		var sl = _El( 'CsnoOpacity' );
		sl.SetPanelEvent( 'onvaluechanged', function()
		{
			_El( 'CsnoOpacityValue' ).text = Math.round( sl.value ) + '%';
			if ( !m_loading )
				_SaveSoon();
		} );
		CsnoBridge.Call( 'upload_get', undefined, function( v )
		{
			_El( 'CsnoUpload' ).checked = v !== '0';
			_El( 'CsnoUploadLabel' ).text = v !== '0' ? '开' : '关';
		} );
		CsnoBridge.CallJson( 'info', undefined, function( i )
		{
			if ( i )
				_El( 'CsnoInfo' ).text = 'CS:NO ' + i.version + ' · ' + i.gpu + ' · 驱动：' + ( i.driver || '系统' );
		} );
	}

	function _SaveSoon()
	{
		if ( m_saveTimer !== null )
			$.CancelScheduled( m_saveTimer );
		m_saveTimer = $.Schedule( 0.4, function()
		{
			m_saveTimer = null;
			var opacity = Math.round( _El( 'CsnoOpacity' ).value ) / 100;
			var dyn = _El( 'CsnoDynamicStick' ).checked ? '1' : '0';
			var hide = _El( 'CsnoHideAll' ).checked ? '1' : '0';
			_SendOpts( opacity, dyn, hide );
		} );
	}

	function _SendOpts( opacity, dyn, hide )
	{
		CsnoBridge.Call( 'touch_opts', opacity + ' ' + dyn + ' ' + hide, null );
		CsnoBridge.Call( 'touch_commit', undefined, null );
	}

	function _OnDynamic()
	{
		var on = _El( 'CsnoDynamicStick' ).checked;
		_El( 'CsnoDynamicLabel' ).text = on ? '开' : '关';
		_SaveSoon();
	}

	// 0.2.3: turning the controls off asks first -- without a keyboard there is no way to play (or to reach this page
	// from a match except the keyboard's Esc)
	function _OnHideAll()
	{
		var on = _El( 'CsnoHideAll' ).checked;
		if ( !on )
		{
			_El( 'CsnoHideAllLabel' ).text = '关';
			_SaveSoon();
			return;
		}
		UiToolkitAPI.ShowGenericPopupTwoOptions( '隐藏全部触控按键？', '对局里屏幕上将不再显示摇杆和按钮，只能用外接键盘鼠标操作（键盘 Esc 打开菜单）。以后可以在这里改回来。', '',
			'隐藏', function() { _El( 'CsnoHideAll' ).checked = true; _El( 'CsnoHideAllLabel' ).text = '开'; _SaveSoon(); },
			'取消', function() { _El( 'CsnoHideAll' ).checked = false; _El( 'CsnoHideAllLabel' ).text = '关'; } );
	}

	function _OnUpload()
	{
		var on = _El( 'CsnoUpload' ).checked;
		if ( on )
		{
			_El( 'CsnoUploadLabel' ).text = '开';
			CsnoBridge.Call( 'upload_set', '1', null );
			return;
		}
		// 0.1.5 b118 (老板): uploading is the condition for playing -- switching it off quits the game
		UiToolkitAPI.ShowGenericPopupTwoOptions( '确定关闭吗？', '关闭诊断日志上传将退出游戏。', '',
			'保持开启', function() { _El( 'CsnoUpload' ).checked = true; _El( 'CsnoUploadLabel' ).text = '开'; },
			'关闭并退出', function() { CsnoBridge.Call( 'upload_set', '0', function() { CsnoBridge.Call( 'quit' ); } ); } );
	}

	function _Reset()
	{
		UiToolkitAPI.ShowGenericPopupTwoOptions( '恢复默认键位', '所有按钮的位置、大小、透明度和名字都会恢复成默认值。', '',
			'恢复默认', function() { CsnoBridge.Call( 'touch_reset', undefined, function() { _Init(); } ); },
			'取消', function() {} );
	}

	function _OpenEditor()
	{
		UiToolkitAPI.ShowCustomLayoutPopup( 'csno_touch_editor', 'file://{resources}/layout/popups/popup_csno_touch_editor.xml' );
	}

	return {
		Init: _Init,
		OnDynamic: _OnDynamic,
		OnHideAll: _OnHideAll,
		OnUpload: _OnUpload,
		Reset: _Reset,
		OpenEditor: _OpenEditor
	};
} )();
