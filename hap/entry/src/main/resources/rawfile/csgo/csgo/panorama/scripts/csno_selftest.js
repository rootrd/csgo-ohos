'use strict';
// CS:NO (09-28, 0.2.0): 一键测试 page (settings_csno_selftest.xml).  CsnoBridge selftest_status:
//   { state: idle|running|uploading|done|failed, step, done, total, run,
//     last: { run, time, shots_ok, shots_total, files_total, files_uploaded, fps, finished } }

var CsnoSelfTest = ( function()
{
	var m_timer = null;

	function _El( id )
	{
		return $.GetContextPanel().FindChildTraverse( id );
	}

	function _Two( n )
	{
		return ( n < 10 ? '0' : '' ) + n;
	}

	function _When( ms )
	{
		if ( !ms )
			return '';
		var d = new Date( ms );
		return ( d.getMonth() + 1 ) + '月' + d.getDate() + '日 ' + _Two( d.getHours() ) + ':' + _Two( d.getMinutes() );
	}

	function _Show( s )
	{
		if ( !s )
			return;
		var last = s.last || {};
		var busy = s.state === 'running' || s.state === 'uploading';
		var state, upload, fps;
		if ( busy )
		{
			state = '正在测试：' + ( s.step || '' ) + ( s.total ? '（' + s.done + '/' + s.total + '）' : '' );
			upload = s.state === 'uploading' ? ( s.step || '上传中' ) : '测试完自动上传';
		}
		else if ( !last.run )
		{
			state = '还没测试过';
			upload = '—';
		}
		else if ( !last.finished )
		{
			state = '上次测试没跑完（' + _When( last.time ) + '）';
			upload = '—';
		}
		else
		{
			state = '已测试（' + _When( last.time ) + '），截图 ' + last.shots_ok + '/' + last.shots_total + ' 张';
			upload = last.files_uploaded >= last.files_total && last.files_total > 0
				? '已全部上传（' + last.files_uploaded + '/' + last.files_total + ' 个文件）'
				: '已上传 ' + last.files_uploaded + '/' + last.files_total + ' 个文件，下次打开游戏会继续上传';
		}
		fps = last.fps ? ( Math.round( parseFloat( last.fps ) * 10 ) / 10 ) + ' 帧/秒' : '—';
		_El( 'CsnoSelfTestState' ).text = state;
		_El( 'CsnoSelfTestUpload' ).text = upload;
		_El( 'CsnoSelfTestFps' ).text = fps;
		_El( 'CsnoSelfTestStart' ).enabled = !busy;
		if ( s.state === 'failed' )
			_El( 'CsnoSelfTestHint' ).text = '上次测试出错：' + ( s.step || '' );
	}

	function _Refresh()
	{
		m_timer = null;
		var root = $.GetContextPanel();
		if ( !root || !root.IsValid() )
			return;
		CsnoBridge.CallJson( 'selftest_status', undefined, function( s ) { _Show( s ); } );
		m_timer = $.Schedule( 2.0, _Refresh );
	}

	function _Init()
	{
		if ( m_timer === null )
			_Refresh();
	}

	function _Start()
	{
		UiToolkitAPI.ShowGenericPopupTwoOptions( '开始一键测试',
			'游戏会自动进地图跑约 2 分钟，期间触摸屏幕无效（不用管它）；跑完自动回到主菜单并上传结果。\n上传约 1~2 MB（截图已压缩）。', '',
			'开始', function()
			{
				CsnoBridge.Call( 'selftest_start', undefined, function( r )
				{
					if ( r && r !== 'ok' )
						_El( 'CsnoSelfTestHint' ).text = r === 'busy' ? '测试已经在进行中' : '请在主菜单开始测试（' + r + '）';
					else
						_El( 'CsnoSelfTestHint' ).text = '测试开始，稍等会自动进入地图…';
				} );
			},
			'取消', function() {} );
	}

	return { Init: _Init, Start: _Start };
} )();
