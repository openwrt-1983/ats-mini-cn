#ifndef PAGE_CONFIG_H
#define PAGE_CONFIG_H

#include <Arduino.h>

static const char pageConfigTitle[] PROGMEM = "ATS-Mini 设置";

static const char pageConfig[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
<FORM ACTION="/setconfig" METHOD="POST" ENCTYPE="multipart/form-data" ONSUBMIT="browserDateTime(true)">
  <TABLE COLUMNS="2">
    <TR><TH COLSPAN="2" CLASS="HEADING">日期时间</TH></TR>
    <TR><TD CLASS="LABEL">使用浏览器时间</TD><TD><INPUT TYPE="CHECKBOX" ID="browserdatetime" ONCHANGE="browserDateTime()"></TD></TR>
    <TR><TD CLASS="LABEL">UTC 日期时间</TD><TD><INPUT TYPE="TEXT" ID="datetime" NAME="datetime" PLACEHOLDER="YYYY-mm-dd HH:MM:SS"></TD></TR>
    <TR><TD CLASS="LABEL">时区</TD><TD><SELECT ID="utcoffset" NAME="utcoffset">{{{utc_options}}}</SELECT></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">界面</TH></TR>
    <TR><TD CLASS="LABEL">界面布局</TD><TD><SELECT NAME="uilayout">{{{layout_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">主题</TD><TD><SELECT NAME="theme">{{{theme_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">反向滚动</TD><TD><INPUT TYPE="CHECKBOX" NAME="scroll" VALUE="on" {{scroll}}></TD></TR>
    <TR><TD CLASS="LABEL">放大菜单</TD><TD><INPUT TYPE="CHECKBOX" NAME="zoom" VALUE="on" {{zoom}}></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">收音机</TH></TR>
    <TR><TD CLASS="LABEL">RDS</TD><TD><SELECT NAME="rds">{{{rds_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">FM 地区</TD><TD><SELECT NAME="fmregion">{{{region_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">FM 立体声</TD><TD><SELECT NAME="fmstereo">{{{stereo_options}}}</SELECT></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">电源</TH></TR>
    <TR><TD CLASS="LABEL">背光亮度</TD><TD><INPUT TYPE="NUMBER" NAME="brightness" MIN="10" MAX="255" STEP="5" VALUE="{{brightness}}" REQUIRED></TD></TR>
    <TR><TD CLASS="LABEL">休眠定时</TD><TD><INPUT TYPE="NUMBER" NAME="sleeptimer" MIN="0" MAX="255" STEP="5" VALUE="{{sleep_timer}}" REQUIRED><BR><SMALL>单位秒；0 表示不自动休眠</SMALL></TD></TR>
    <TR><TD CLASS="LABEL">休眠方式</TD><TD><SELECT NAME="sleepmode">{{{sleep_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">关机定时</TD><TD><INPUT TYPE="NUMBER" NAME="offtimer" MIN="0" MAX="240" STEP="5" VALUE="{{off_timer}}" REQUIRED><BR><SMALL>单位分钟；0 表示不自动关机。修改后会重新计时。</SMALL></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">连接</TH></TR>
    <TR><TD CLASS="LABEL">USB 模式</TD><TD><SELECT NAME="usbmode">{{{usb_options}}}</SELECT></TD></TR>
    <TR><TD CLASS="LABEL">TCP Mode</TD><TD><SELECT NAME="tcpmode">{{{tcp_options}}}</SELECT><BR><SMALL>监听 60000 端口；需要 Wi-Fi</SMALL></TD></TR>
    <TR><TD CLASS="LABEL">蓝牙模式</TD><TD><SELECT NAME="blemode">{{{ble_options}}}</SELECT></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">硬件</TH></TR>
    <TR><TD CLASS="LABEL">音频钳位</TD><TD><INPUT TYPE="CHECKBOX" NAME="audioclamp" VALUE="on" {{audio_clamp}}><BR><SMALL>接收机带音频钳位电路时开启</SMALL></TD></TR>
    <TR><TD CLASS="LABEL">编码器半步解码</TD><TD><INPUT TYPE="CHECKBOX" NAME="encoderhalfstep" VALUE="on" {{half_step}}><BR><SMALL>30 定位点 / 15 电周期请开启；20 定位点（EC11 等）请关闭。设备设置菜单的「编码器」是同一设置</SMALL></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">开机画面</TH></TR>
    <TR><TD CLASS="LABEL">当前图片</TD><TD>{{{splash}}}</TD></TR>
    <TR>
      <TD CLASS="LABEL">Upload PNG</TD>
      <TD><INPUT TYPE="FILE" NAME="splash" ACCEPT=".png"><BR><SMALL>要求分辨率：{{resolution}} 像素；最大 512 KB</SMALL></TD>
    </TR>
    <TR><TD CLASS="LABEL">删除图片</TD><TD><INPUT TYPE="CHECKBOX" NAME="deletesplash" VALUE="on"></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING"><INPUT TYPE="SUBMIT" VALUE="保存"></TH></TR>
  </TABLE>
</FORM>
<SCRIPT>
function browserDateTime(submit) {
  const enabled = document.getElementById('browserdatetime').checked;
  const dateTime = document.getElementById('datetime');
  const utcOffset = document.getElementById('utcoffset');
  if(enabled) {
    const now = new Date();
    dateTime.value = now.toISOString().slice(0,19).replace('T', ' ');
    const minutes = -now.getTimezoneOffset();
    for(const option of utcOffset.options)
      if(Number(option.dataset.minutes) === minutes) utcOffset.value = option.value;
  }
  dateTime.disabled = utcOffset.disabled = enabled && !submit;
}
</SCRIPT>
<H1>配置备份</H1>
<P>把收音机当前的全部设置导出成一个文本文件：音量、亮度、主题、波段、SSB 校准、VFO、全部电台记忆、Wi-Fi 与 Web 登录。重刷固件会擦掉这些数据，刷完后在这里上传该文件即可原样恢复，不用再逐项设置。</P>
<TABLE COLUMNS="2">
  <TR><TD CLASS="LABEL">导出</TD><TD><A HREF="/backup.txt">下载配置文件</A><BR><SMALL>纯文本，可直接用记事本打开修改；文件内含 Wi-Fi 与 Web 登录密码，请勿外传</SMALL></TD></TR>
</TABLE>
<FORM ACTION="/restore" METHOD="POST" ENCTYPE="multipart/form-data">
  <TABLE COLUMNS="2">
    <TR><TD CLASS="LABEL">导入</TD><TD><INPUT TYPE="FILE" NAME="backup" ACCEPT=".txt,.conf,text/plain" REQUIRED><BR><SMALL>上传后设备会写入配置并自动重启；设置项按 key 覆盖，文件里没有的项目保持当前值</SMALL></TD></TR>
    <TR><TH COLSPAN="2"><INPUT TYPE="SUBMIT" VALUE="导入并重启"></TH></TR>
  </TABLE>
</FORM>
<P><SMALL>开机画面（Splash）由 <A HREF="/splash.png">这里</A> 单独下载，重刷后到上面的表单里重新上传。</SMALL></P>
)HTML";

static const char pageRestoreTitle[] PROGMEM = "导入配置";

static const char pageRestore[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
<P>{{message}}</P>
<P><A HREF="/config">返回设置</A></P>
)HTML";

static const char pageSplash[] PROGMEM = R"HTML(<IMG SRC="/splash.png?{{version}}" ALT="当前开机画面" STYLE="max-width:100%;height:auto;">)HTML";

static const char pageConfigThemeOption[] PROGMEM = R"HTML(<OPTION VALUE="{{value}}" {{selected}}>{{label}}</OPTION>)HTML";

static const char pageConfigUtcOption[] PROGMEM = R"HTML(<OPTION VALUE="{{value}}" DATA-MINUTES="{{minutes}}" {{selected}}>{{label}}</OPTION>)HTML";

#endif // PAGE_CONFIG_H
