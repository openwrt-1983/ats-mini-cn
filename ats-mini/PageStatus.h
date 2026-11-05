#ifndef PAGE_STATUS_H
#define PAGE_STATUS_H

#include <Arduino.h>

static const char pageStatusTitle[] PROGMEM = "ATS-Mini 状态";

static const char pageStatus[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
<TABLE COLUMNS="2">
  <TR><TD CLASS="LABEL">IP 地址</TD><TD><A HREF="http://{{ip}}">{{ip}}</A> ({{ssid}})</TD></TR>
  <TR><TD CLASS="LABEL">MAC 地址</TD><TD>{{mac}}</TD></TR>
  <TR><TD CLASS="LABEL">固件版本</TD><TD>{{version}}</TD></TR>
  <TR><TD CLASS="LABEL">日期时间</TD><TD>{{time}}</TD></TR>
  <TR><TD CLASS="LABEL">波段</TD><TD>{{band}}</TD></TR>
  <TR><TD CLASS="LABEL">频率</TD><TD>{{frequency}}{{mode}}</TD></TR>
  <TR><TD CLASS="LABEL">信号强度</TD><TD>{{rssi}}dBuV</TD></TR>
  <TR><TD CLASS="LABEL">信噪比</TD><TD>{{snr}}dB</TD></TR>
  <TR><TD CLASS="LABEL">电池电压</TD><TD>{{battery}}V</TD></TR>
</TABLE>
<P CLASS="CENTER"><A HREF="/screenshot.bmp">下载屏幕截图</A></P>
)HTML";

#endif // PAGE_STATUS_H
