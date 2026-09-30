'use strict';
// CS:NO (09-28, 0.1.5): the app's one-time notices, shown as CS:GO popups in the main menu instead of Android
// dialogs (老板: Java 不要界面).  CsnoBridge.java "notices": [{ id, title, text, upload }].
//   upload = the diagnostic-log notice: 同意上传 / 不上传 (answer stored by the app).

var CsnoMainMenu = ( function()
{
	var m_waits = 0;
	var m_shown = false;

	var m_busy = false;     // a chain of notices is on screen (the poll below must not start a second one)

	function _ShowNext( list )
	{
		if ( !list || list.length === 0 )
		{
			m_busy = false;
			return;
		}
		m_busy = true;
		var n = list.shift();
		var next = function() { $.Schedule( 0.3, function() { _ShowNext( list ); } ); };
		if ( n.upload )
		{
			// 0.1.5 b118 (老板): uploading is the condition for playing -- "不同意" asks once more, then quits
			var agree = function() { CsnoBridge.Call( 'upload_set', '1' ); CsnoBridge.Call( 'notice_ack', n.id ); next(); };
			UiToolkitAPI.ShowGenericPopupTwoOptions( n.title, n.text, '',
				'同意', agree,
				'不同意', function()
				{
					$.Schedule( 0.2, function()
					{
						UiToolkitAPI.ShowGenericPopupTwoOptions( '确定不同意吗？', '不同意上传诊断日志将退出游戏。', '',
							'同意上传', agree,
							'不同意并退出', function() { CsnoBridge.Call( 'quit' ); } );
					} );
				} );
		}
		else
		{
			UiToolkitAPI.ShowGenericPopupOk( n.title, n.text, '',
				function() { CsnoBridge.Call( 'notice_ack', n.id ); next(); },
				function() { CsnoBridge.Call( 'notice_ack', n.id ); next(); } );
		}
	}

	// another popup up (the server announcement from csno_notice.cpp, a CS:GO message box)?  Ours waits behind it
	function _OtherPopupOpen()
	{
		var pm = $.GetContextPanel().FindChildTraverse( 'PopupManager' );
		if ( !pm )
			return false;
		for ( var i = 0; i < pm.GetChildCount(); i++ )
		{
			// the manager's own DimBackground / BlurBackground are always there; popups are Popup* panels
			var c = pm.GetChild( i );
			if ( c && c.visible && String( c.paneltype ).indexOf( 'Popup' ) === 0 )
				return true;
		}
		return false;
	}

	function _Check()
	{
		if ( m_shown )
			return;
		// one after the other, not stacked: wait (up to 5 min) until no other popup is open
		if ( _OtherPopupOpen() && ( m_waits++ ) < 600 )
		{
			$.Schedule( 0.5, _Check );
			return;
		}
		m_shown = true;
		CsnoBridge.CallJson( 'notices', undefined, function( list )
		{
			if ( list && list.length )
				_ShowNext( list );
			$.Schedule( 5.0, _Poll );
		} );
	}

	// 0.2.0: notices that come later (一键测试完成 after the self test is back in the main menu): look every 5 s,
	// only while no popup is up (a shown notice stays in the list until it is answered)
	function _Poll()
	{
		var root = $.GetContextPanel();
		if ( !root || !root.IsValid() )
			return;
		if ( m_busy || _OtherPopupOpen() || !root.visible )
		{
			$.Schedule( 5.0, _Poll );
			return;
		}
		CsnoBridge.CallJson( 'notices', undefined, function( list )
		{
			if ( list && list.length && !m_busy && !_OtherPopupOpen() )
				_ShowNext( list );
			$.Schedule( 5.0, _Poll );
		} );
	}

	// 0.1.5 b111: the picture runs under the camera cutout; only the side bar (settings etc.) moved out of it.
	// 0.2.1 (老板 09-29: 主界面菜单为啥不紧靠最左边而是留白): the side bar sits at the left edge again.
	var CUTOUT_SHIFT = false;
	var m_cutTries = 0;
	function _Cutout()
	{
		if ( !CUTOUT_SHIFT )
			return;
		CsnoBridge.Call( 'safe_insets', undefined, function( t )
		{
			var v = ( t || '' ).split( ',' ).map( function( x ) { return parseInt( x, 10 ) || 0; } );
			if ( v.length < 4 || v[ 3 ] <= 0 )
			{
				if ( ( m_cutTries++ ) < 10 )             // the app measures once the window has focus
					$.Schedule( 1.0, _Cutout );
				return;
			}
			var n = Math.round( v[ 0 ] * 1080 / v[ 3 ] / 4 ) * 4;
			n = Math.max( 0, Math.min( 240, n ) );
			if ( n > 0 )
				$.GetContextPanel().AddClass( 'csno-cut-' + n );
		} );
	}

	return { Check: _Check, Cutout: _Cutout };
} )();

( function()
{
	// a moment after the main menu is up: the server announcement (csno_notice.cpp) shows once the server answered,
	// ~3 s in; ours comes after it, never stacked on it
	$.Schedule( 6.0, CsnoMainMenu.Check );
	$.Schedule( 0.5, CsnoMainMenu.Cutout );
} )();
