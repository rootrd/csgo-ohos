'use strict';
// CS:NO (09-28, 0.1.5): calls into the Android app from Panorama.
//   CsnoBridge.Call( 'touch_get', undefined, function( reply ) { ... } );
// -> console "csno_java <seq> <verb> [encodeURIComponent(arg)]" (game/client/cstrike15/csno_bridge.cpp) -> the app
// (CsnoBridge.java); the reply comes back in the convar csno_java_reply as "<seq>|<text>".  The convar holds one
// reply, so calls are queued and sent one at a time.

var CsnoBridge = ( function()
{
	var m_seq = 0;
	var m_queue = [];
	var m_busy = null;          // { seq, cb, sent, tries }

	function _Call( verb, arg, cb )
	{
		m_queue.push( { verb: verb, arg: arg, cb: cb } );
		if ( !m_busy )
			_Next();
	}

	function _Next()
	{
		if ( m_queue.length === 0 )
		{
			m_busy = null;
			return;
		}
		var job = m_queue.shift();
		m_seq = ( m_seq % 100000 ) + 1;
		var line = 'csno_java ' + m_seq + ' ' + job.verb;
		if ( job.arg !== undefined && job.arg !== null )
			line += ' ' + encodeURIComponent( String( job.arg ) );
		m_busy = { seq: String( m_seq ), cb: job.cb, tries: 0 };
		GameInterfaceAPI.ConsoleCommand( line );
		$.Schedule( 0.02, _Poll );
	}

	function _Poll()
	{
		if ( !m_busy )
			return;
		var reply = GameInterfaceAPI.GetSettingString( 'csno_java_reply' ) || '';
		var bar = reply.indexOf( '|' );
		if ( bar > 0 && reply.substring( 0, bar ) === m_busy.seq )
		{
			var cb = m_busy.cb;
			var text = reply.substring( bar + 1 );
			m_busy = null;
			if ( cb )
			{
				try { cb( text ); } catch ( e ) { $.Msg( 'CSNO_BRIDGE callback: ' + e ); }
			}
			_Next();
			return;
		}
		if ( ++m_busy.tries > 150 )          // ~3 s: the app never answered (old app?)
		{
			$.Msg( 'CSNO_BRIDGE: no reply for seq ' + m_busy.seq );
			var cb2 = m_busy.cb;
			m_busy = null;
			if ( cb2 )
			{
				try { cb2( '' ); } catch ( e ) { }
			}
			_Next();
			return;
		}
		$.Schedule( 0.02, _Poll );
	}

	function _CallJson( verb, arg, cb )
	{
		_Call( verb, arg, function( text )
		{
			var obj = null;
			try { obj = JSON.parse( text ); } catch ( e ) { obj = null; }
			cb( obj );
		} );
	}

	return {
		Call: _Call,
		CallJson: _CallJson
	};
} )();
