'use strict';
// CS:NO (09-29, 0.2.1): 问题反馈 page (settings_csno_feedback.xml).  The text goes to the app in pieces (a console
// line holds ~500 bytes; encodeURIComponent makes a Chinese character 9): feedback_begin, feedback_part x n,
// feedback_send -> "ok" | "empty" | "too_many" | "error ...".

var CsnoFeedback = ( function()
{
	var m_sending = false;

	function _El( id )
	{
		return $.GetContextPanel().FindChildTraverse( id );
	}

	function _Count()
	{
		var root = $.GetContextPanel();
		if ( !root || !root.IsValid() )
			return;
		var t = _El( 'CsnoFeedbackText' );
		if ( t )
			_El( 'CsnoFeedbackCount' ).text = ( t.text || '' ).length + ' / 500';
		$.Schedule( 0.5, _Count );
	}

	function _Init()
	{
		_Count();
	}

	function _Send()
	{
		if ( m_sending )
			return;
		var text = ( _El( 'CsnoFeedbackText' ).text || '' ).trim();
		if ( !text )
		{
			_El( 'CsnoFeedbackHint' ).text = '先写一下遇到的问题再提交。';
			return;
		}
		m_sending = true;
		_El( 'CsnoFeedbackSend' ).enabled = false;
		_El( 'CsnoFeedbackHint' ).text = '正在提交…';
		CsnoBridge.Call( 'feedback_begin' );
		for ( var i = 0; i < text.length; i += 40 )
			CsnoBridge.Call( 'feedback_part', text.substring( i, i + 40 ) );
		CsnoBridge.Call( 'feedback_send', undefined, function( r )
		{
			m_sending = false;
			_El( 'CsnoFeedbackSend' ).enabled = true;
			if ( r === 'ok' )
			{
				_El( 'CsnoFeedbackText' ).text = '';
				_El( 'CsnoFeedbackHint' ).text = '已提交，谢谢！日志正在后台上传（没网的话下次打开游戏会自动补传）。';
			}
			else if ( r === 'too_many' )
				_El( 'CsnoFeedbackHint' ).text = '一小时内最多提交 5 次，稍后再试。';
			else
				_El( 'CsnoFeedbackHint' ).text = '提交失败：' + ( r || '没有响应' );
		} );
	}

	return { Init: _Init, Send: _Send };
} )();
