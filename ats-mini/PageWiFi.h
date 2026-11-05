#ifndef PAGE_WIFI_H
#define PAGE_WIFI_H

#include <Arduino.h>

static const char pageWiFiTitle[] PROGMEM = "ATS-Mini Wi-Fi";

static const char pageWiFi[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
<FORM ACTION="/wifi" METHOD="POST">
  <TABLE COLUMNS="2">
    <TR><TH COLSPAN="2" CLASS="HEADING">连接</TH></TR>
    <TR><TD CLASS="LABEL">Wi-Fi 模式</TD><TD><SELECT NAME="wifimode">{{{wifi_options}}}</SELECT><BR><SMALL>切换模式可能导致本页面断开。“关”和“仅对时”会停用网页界面。</SMALL></TD></TR>
    <TR><TD CLASS="LABEL">扫描隐藏 SSID</TD><TD><INPUT TYPE="CHECKBOX" NAME="wifiscanhidden" VALUE="on" {{scan_hidden}}></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">Wi-Fi 网络 1</TH></TR>
    <TR><TD CLASS="LABEL">SSID</TD><TD><INPUT TYPE="TEXT" NAME="wifissid1" VALUE="{{ssid1}}"></TD></TR>
    <TR><TD CLASS="LABEL">Password</TD><TD><INPUT TYPE="PASSWORD" NAME="wifipass1" VALUE="{{pass1}}"></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">Wi-Fi 网络 2</TH></TR>
    <TR><TD CLASS="LABEL">SSID</TD><TD><INPUT TYPE="TEXT" NAME="wifissid2" VALUE="{{ssid2}}"></TD></TR>
    <TR><TD CLASS="LABEL">Password</TD><TD><INPUT TYPE="PASSWORD" NAME="wifipass2" VALUE="{{pass2}}"></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">Wi-Fi 网络 3</TH></TR>
    <TR><TD CLASS="LABEL">SSID</TD><TD><INPUT TYPE="TEXT" NAME="wifissid3" VALUE="{{ssid3}}"></TD></TR>
    <TR><TD CLASS="LABEL">Password</TD><TD><INPUT TYPE="PASSWORD" NAME="wifipass3" VALUE="{{pass3}}"></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING">网页界面登录凭据</TH></TR>
    <TR><TD CLASS="LABEL">用户名</TD><TD><INPUT TYPE="TEXT" NAME="username" VALUE="{{username}}"></TD></TR>
    <TR><TD CLASS="LABEL">Password</TD><TD><INPUT TYPE="PASSWORD" NAME="password" VALUE="{{password}}"></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING"><INPUT TYPE="SUBMIT" VALUE="保存"></TH></TR>
  </TABLE>
</FORM>
)HTML";

#endif // PAGE_WIFI_H
