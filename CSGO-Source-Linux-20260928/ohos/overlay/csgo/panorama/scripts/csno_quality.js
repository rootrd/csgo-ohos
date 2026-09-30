'use strict';
// CS:NO (09-28, 0.2.0): 画质预设 page (settings_csno_quality.xml).  CsnoBridge quality_status:
//   { current: high|mid|low|lowest|"", recommended, res, presets: [{ id, name, res }] }

var CsnoQuality = ( function()
{
	var NAMES = { high: '高', mid: '中', low: '低', lowest: '极低' };
	var DESC = {
		high: '2 倍抗锯齿 · 中等阴影/特效/贴图',
		mid: 'FXAA · 最低阴影/特效 · 中等贴图',
		low: '无抗锯齿 · 全部最低',
		lowest: '无抗锯齿 · 全部最低 · 最省电'
	};

	function _El( id )
	{
		return $.GetContextPanel().FindChildTraverse( id );
	}

	function _Show( s )
	{
		if ( !s || !s.presets )
			return;
		var cur = s.current ? NAMES[ s.current ] + '（' + s.res.replace( 'x', '×' ) + '）' : '自定义（' + ( s.res || '—' ).replace( 'x', '×' ) + '）';
		_El( 'CsnoQualityCurrent' ).text = cur;
		_El( 'CsnoQualityRec' ).text = NAMES[ s.recommended ] || '—';
		var hz = _El( 'CsnoQualityHz' );
		if ( hz && s.hz )
		{
			hz.text = s.hz + ' Hz' + ( s.maxhz && s.hz + 1 < s.maxhz
				? '（屏幕最高 ' + s.maxhz + ' Hz，被系统限住了：去 手机设置 → 显示 → 屏幕刷新率，把 CS:NO 设成高刷 / 最高）'
				: '' );
		}
		s.presets.forEach( function( p )
		{
			var l = _El( 'CsnoQualityLabel_' + p.id );
			if ( l )
				l.text = p.name + '　' + p.res.replace( 'x', '×' ) + '　' + DESC[ p.id ] +
					( p.id === s.current ? '　（当前）' : '' ) + ( p.id === s.recommended ? '　★推荐' : '' );
		} );
	}

	function _Init()
	{
		CsnoBridge.CallJson( 'quality_status', undefined, function( s ) { _Show( s ); } );
	}

	function _Pick( id )
	{
		UiToolkitAPI.ShowGenericPopupTwoOptions( '换成「' + NAMES[ id ] + '」画质？',
			'游戏会自动重启一次（约 10~20 秒）后生效。', '',
			'确定', function()
			{
				_El( 'CsnoQualityHint' ).text = '正在应用，游戏马上重启…';
				CsnoBridge.Call( 'quality_set', id, function( r )
				{
					if ( r && r !== 'ok' )
						_El( 'CsnoQualityHint' ).text = '没能应用：' + r;
				} );
			},
			'取消', function() {} );
	}

	return { Init: _Init, Pick: _Pick };
} )();
