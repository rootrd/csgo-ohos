var MobileMenu = (function () {
    'use strict';
    var root = $.GetContextPanel();
    function t(text) { return root.LocalizeText(text); }
    var revision = -1, page = -1, selectedMap = 0, mode = 0, bots = 8, difficulty = 1, team = 3;
    var settings = [], maps = [], updating = false, choices = {};
    function label(parent, text, cls) {
        var p = $.CreatePanel('Label', parent, ''); p.text = t(text); if (cls) p.AddClass(cls); return p;
    }
    function button(parent, text, cls, click) {
        var p = $.CreatePanel('Button', parent, ''); p.AddClass(cls); label(p, text); p.SetPanelEvent('onactivate', click); return p;
    }
    function buildChoices(id, entries, set) {
        var parent = $('#' + id); parent.RemoveAndDeleteChildren(); choices[id] = [];
        entries.forEach(function (entry) {
            var p = button(parent, entry[1], 'choice', function () { set(entry[0]); updateChoices(); });
            choices[id].push({panel:p, value:entry[0]});
        });
    }
    function updateChoices() {
        var values = {ModeChoices:mode, BotChoices:bots, DifficultyChoices:difficulty, TeamChoices:team, LanguageChoices:root.GetGameLanguage()};
        Object.keys(values).forEach(function (id) { choices[id].forEach(function (c) { c.panel.SetHasClass('chosen', c.value === values[id]); }); });
        maps.forEach(function (p,i) { p.SetHasClass('chosen', i === selectedMap); });
        $('#ChosenMap').text = root.GetMapName(selectedMap) || t('没有找到可用地图');
        $('#StartMatch').enabled = root.GetMapCount() > 0;
    }
    function buildMaps() {
        var parent = $('#MapList'); parent.RemoveAndDeleteChildren(); maps = [];
        for (var i=0; i<root.GetMapCount(); ++i) (function(index) {
            maps.push(button(parent, root.GetMapName(index), 'map-button', function () {selectedMap=index; updateChoices();}));
        })(i);
        if (!maps.length) label(parent, '未找到地图。请检查游戏资源中的 maps 目录。', 'description');
    }
    function settingText(index,value) {
        if (index === 0 || index === 1 || index === 8) return Math.round(value*100) + '%';
        if (index === 2) return Math.round(value) + ' FPS';
        if (index === 3) return t(['流畅','均衡','清晰'][Math.round(value)]);
        if (index === 4) return t(['标准','2×','4×','8×','16×'][Math.round(value)]);
        if (index === 5) return t(['低','中','高','很高'][Math.round(value)]);
        if (index === 7 || index === 9 || index === 11) return t(value > .5 ? '开启' : '关闭');
        if (index === 12) return t(['关闭','开镜','始终'][Math.round(value)] || '关闭');
        if (index === 10) return Math.round(value) + '%';
        return value.toFixed(2) + '×';
    }
    function buildSettings() {
        settings = [];
        for(var i=0;i<root.GetSettingCount();++i) (function(index) {
            $('#SettingLabel'+index).text=root.GetSettingLabel(index);
            var slider=$('#Setting'+index);
            slider.min=root.GetSettingMin(index); slider.max=root.GetSettingMax(index); slider.increment=root.GetSettingStep(index);
            var value=$('#SettingValue'+index);
            slider.SetPanelEvent('onvaluechanged',function(){if(!updating){root.SetSetting(index,slider.value);value.text=settingText(index,root.GetSetting(index));}});
            settings.push({slider:slider,value:value});
        })(i);
    }
    function refreshSettings() {
        updating=true;
        settings.forEach(function(s,i){s.slider.SetValueNoEvents(root.GetSetting(i));s.value.text=settingText(i,root.GetSetting(i));});
        updating=false;
    }
    function refresh() {
        var next = root.GetRevision();
        if (next !== revision) {
            revision=next; page=root.GetPage();
            var inMatch=root.IsInMatch();
            root.SetHasClass('in-match',inMatch);
            root.SetHasClass('on-home',page===0);
            $('#NavHome').SetHasClass('active-tab',page===0);
            $('#NavPlay').SetHasClass('active-tab',page===1);
            $('#NavSettings').SetHasClass('active-tab',page===2);
            $('#NavLayout').SetHasClass('active-tab',page===3);
            $('#NavPlay').visible=!inMatch;
            $('#HomePage').visible=page===0; $('#PlayPage').visible=page===1; $('#SettingsPage').visible=page===2;
            $('#ConfirmPage').visible=page===5||page===6;
            $('#ResumeButton').visible=inMatch; $('#PlayButton').visible=!inMatch;
            $('#DisconnectButton').visible=inMatch; $('#TeamButtons').visible=inMatch;
            $('#PageTitle').text=t([inMatch?'游戏菜单':'本地对战','开始游戏','设置','触屏布局','载入中','退出游戏','返回主菜单'][page]||'本地对战');
            $('#HomeHeadline').text=t(inMatch?'对局进行中':'与电脑玩家练习');
            $('#HomeMapName').text=root.GetMapName(root.GetSelectedMap())||t('选择地图');
            $('#HomeMatchSummary').text=t(['休闲','竞技','死亡竞赛'][root.GetMode()]||'休闲')+' · '+root.GetBots()+t(' 名机器人');
            $('#Status').text=root.GetMessage(); $('#EditorStatus').text=root.GetMessage();
            if(page===1){selectedMap=root.GetSelectedMap();mode=root.GetMode();bots=root.GetBots();difficulty=root.GetDifficulty();team=root.GetTeam();buildMaps();updateChoices();}
            if(page===2){refreshSettings();updateChoices();}
            if(page===3){
                updating=true;
                $('#SelectedControl').text=t('当前：')+root.GetControlLabel(root.GetSelectedControl());
                $('#ButtonSize').max=root.GetSelectedControl()===0?.5:.3;
                $('#ButtonSize').SetValueNoEvents(root.GetControlSize()); $('#ButtonOpacity').SetValueNoEvents(root.GetControlOpacity());
                updating=false;
            }
            if(page===5||page===6){$('#ConfirmTitle').text=t(page===5?'退出游戏？':'结束当前对局？');$('#ConfirmText').text=t(page===5?'已保存的设置和按键布局会保留。':'将离开当前地图并返回主菜单。');}
        }
        $.Schedule(.1,refresh);
    }
    function action(name) {root.Action(name);refreshNow();}
    function refreshNow(){revision=-1;}
    buildChoices('ModeChoices',[[0,'休闲'],[1,'竞技'],[2,'死亡竞赛']],function(v){mode=v;});
    buildChoices('BotChoices',[[0,'0'],[4,'4'],[8,'8'],[12,'12'],[20,'20']],function(v){bots=v;});
    buildChoices('DifficultyChoices',[[0,'简单'],[1,'普通'],[2,'困难'],[3,'专家']],function(v){difficulty=v;});
    buildChoices('TeamChoices',[[2,'恐怖分子 T'],[3,'反恐精英 CT']],function(v){team=v;});
    buildChoices('LanguageChoices',[[0,'跟随系统'],[1,'简体中文'],[2,'English']],function(v){root.SetGameLanguage(v);refreshNow();});
    buildSettings();
    $('#ButtonSize').SetPanelEvent('onvaluechanged',function(){if(!updating)root.SetControlSize($('#ButtonSize').value);});
    $('#ButtonOpacity').SetPanelEvent('onvaluechanged',function(){if(!updating)root.SetControlOpacity($('#ButtonOpacity').value);});
    $.Schedule(0,refresh);
    $.Msg('MOBILE_UI_SCRIPT: initialized, page=',root.GetPage(),', maps=',root.GetMapCount());
    return {
        Action:action, Back:function(){action('back');}, Apply:function(){action('apply_settings');},
        Start:function(){root.StartMatch(selectedMap,mode,bots,difficulty,team);refreshNow();},
        Confirm:function(){action(page===5?'confirm_quit':'confirm_disconnect');}
    };
})();
