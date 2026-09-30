'use strict';
// CS:NO boot package: the resource download page.  The app downloads (CsnoDownload.java); this page only shows it:
// every 0.5 s CsnoBridge "dl_status" -> { state, phaseLabel, frac, wireDone, wireTotal, diskTotal, filesDone,
// filesTotal, speed, eta, file, note, msg, canRetry, retryIn, free, wireApprox, diskApprox }.
//   state: running / retry_wait / failed / done / idle

var CsnoBoot = ( function()
{
	var DEFAULT_NOTE = '网络传输的是压缩包，装到手机上会解压变大。请连接 Wi-Fi、保持亮屏；中途退出没关系，下次打开 CS:NO 会接着下载。';
	var m_state = '';
	var m_polling = false;
	var m_leaving = false;

	function _El( id ) { return $.GetContextPanel().FindChildTraverse( id ); }

	function _Gb( b ) { return ( b / 1e9 ).toFixed( 2 ); }

	function _Show( id, bShow ) { var p = _El( id ); if ( p ) p.SetHasClass( 'CsnoHidden', !bShow ); }

	function _Init()
	{
		$.Msg( 'CSNO_BRIDGE:ready' );
		_Show( 'CsnoRetry', false );
		_Show( 'CsnoEnter', false );
		_El( 'CsnoNote' ).text = DEFAULT_NOTE;
		CsnoBridge.Call( 'boot_ready', undefined, null );
		if ( !m_polling )
		{
			m_polling = true;
			_Poll();
		}
	}

	function _Poll()
	{
		if ( m_leaving )
			return;
		CsnoBridge.CallJson( 'dl_status', undefined, function( s )
		{
			if ( s )
				_Update( s );
			$.Schedule( 0.5, _Poll );
		} );
	}

	function _Update( s )
	{
		m_state = s.state || '';
		var measuring = !s.wireTotal || s.phase === 'CONNECTING' || s.phase === 'MANIFEST';
		var done = m_state === 'done';
		var failed = m_state === 'failed';
		var waiting = m_state === 'retry_wait';

		var frac = done ? 1 : ( s.frac || 0 );
		_El( 'CsnoBarFill' ).style.width = ( measuring && !done ? 0 : frac * 100 ).toFixed( 2 ) + '%';
		_El( 'CsnoBarFill' ).GetParent().SetHasClass( 'CsnoBarWaiting', failed || waiting );

		if ( done )
		{
			_El( 'CsnoPhase' ).text = '下载完成';
			_El( 'CsnoPercent' ).text = '100%';
			_El( 'CsnoAmount' ).text = '游戏资源已全部下载并校验完成';
			_El( 'CsnoSpeed' ).text = '';
			_El( 'CsnoFile' ).text = '';
			_El( 'CsnoNote' ).text = '点「进入游戏」开始。';
		}
		else if ( failed )
		{
			_El( 'CsnoPhase' ).text = '下载暂停';
			_El( 'CsnoSpeed' ).text = '';
			_El( 'CsnoNote' ).text = s.msg || '下载出错了。';
		}
		else if ( waiting )
		{
			_El( 'CsnoPhase' ).text = '网络中断，' + ( s.retryIn || 5 ) + ' 秒后自动重试（' + ( s.autoRetries || 1 ) + '/3）';
			_El( 'CsnoSpeed' ).text = '';
			_El( 'CsnoNote' ).text = s.msg || DEFAULT_NOTE;
		}
		else
		{
			_El( 'CsnoPhase' ).text = s.phaseLabel || '正在连接 Steam';
			_El( 'CsnoPercent' ).text = measuring ? '准备中' : ( frac * 100 ).toFixed( 1 ) + '%';
			_El( 'CsnoAmount' ).text = measuring ? '正在连接 Steam 服务器…'
				: '已下载 ' + _Gb( s.wireDone ) + ' / ' + _Gb( s.wireTotal ) + ' GB · 文件 ' + s.filesDone + ' / ' + s.filesTotal;
			var sp = '';
			if ( s.speed > 1 && s.phase === 'DOWNLOADING' )
			{
				sp = ( s.speed / 1e6 ).toFixed( 1 ) + ' MB/s';
				if ( s.eta >= 3600 )
					sp += ' · 剩余 ' + Math.floor( s.eta / 3600 ) + ' 小时 ' + Math.floor( ( s.eta % 3600 ) / 60 ) + ' 分';
				else if ( s.eta >= 0 )
					sp += ' · 剩余约 ' + Math.max( 1, Math.floor( s.eta / 60 ) ) + ' 分钟';
			}
			_El( 'CsnoSpeed' ).text = sp;
			_El( 'CsnoFile' ).text = s.file || '';
			_El( 'CsnoNote' ).text = s.note ? s.note : DEFAULT_NOTE;
		}
		_El( 'CsnoNote' ).SetHasClass( 'CsnoNoteError', failed );
		_Show( 'CsnoRetry', failed && !!s.canRetry );
		_Show( 'CsnoEnter', done );

		var wire = s.wireTotal || s.wireApprox || 0, disk = s.diskTotal || s.diskApprox || 0;
		if ( wire > 0 )
			_El( 'CsnoChipWire' ).text = '下载约 ' + ( wire / 1e9 ).toFixed( 1 ) + ' GB';
		if ( disk > 0 )
			_El( 'CsnoChipDisk' ).text = '安装后约 ' + ( disk / 1e9 ).toFixed( 1 ) + ' GB';
		_El( 'CsnoChipFree' ).text = s.free >= 0 ? '可用空间 ' + ( s.free / 1e9 ).toFixed( 1 ) + ' GB' : '';
	}

	function _Retry()
	{
		CsnoBridge.Call( 'dl_retry', undefined, null );
		_Show( 'CsnoRetry', false );
		_El( 'CsnoPhase' ).text = '正在重新连接';
	}

	function _Enter()
	{
		if ( m_state !== 'done' )
			return;
		m_leaving = true;
		_El( 'CsnoPhase' ).text = '正在进入游戏…';
		_Show( 'CsnoEnter', false );
		CsnoBridge.Call( 'dl_restart', undefined, null );
	}

	function _Quit()
	{
		m_leaving = true;
		CsnoBridge.Call( 'quit', undefined, null );
	}

	return { Init: _Init, Retry: _Retry, Enter: _Enter, Quit: _Quit };
} )();

// the layout panels exist when this runs (scripts are evaluated after the XML is built)
CsnoBoot.Init();
