#ifndef PAGE_UPDATE_H
#define PAGE_UPDATE_H

#include <Arduino.h>

static const char pageUpdateTitle[] PROGMEM = "ATS-Mini 固件升级";

static const char pageUpdate[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
<TABLE COLUMNS="1">
  <TR><TD CLASS="CENTER">{{message}}</TD></TR>
  <TR><TH CLASS="HEADING">
    <FORM METHOD="POST" ACTION="/update">
      <BUTTON TYPE="SUBMIT" NAME="action" VALUE="{{action}}" STYLE="padding: 0.5em 2em;" {{disabled}}>{{button}}</BUTTON>
    </FORM>
  </TH></TR>
  <TR><TD CLASS="CENTER">
    <DETAILS><SUMMARY>手动上传固件</SUMMARY>
      <FORM METHOD="POST" ACTION="/update/upload" ENCTYPE="multipart/form-data" ONSUBMIT="this.elements.size.value=this.elements.firmware.files[0].size;this.querySelector('button').disabled=true;">
        <!-- Size must precede the file for the first upload callback. -->
        <INPUT TYPE="HIDDEN" NAME="size">
        <P><INPUT TYPE="FILE" NAME="firmware" ARIA-LABEL="固件文件" ACCEPT=".bin" REQUIRED {{disabled}}></P>
        <SMALL>请选择与接收机型号对应的 <CODE>-ota.bin</CODE> 或 <CODE>ats-mini.ino.bin</CODE> 文件。</SMALL>
        <DIV CLASS="HEADING" STYLE="padding: 0.5em; margin-top: 1em;">
          <BUTTON TYPE="SUBMIT" STYLE="padding: 0.5em 2em;" {{disabled}}>上传</BUTTON>
        </DIV>
      </FORM>
    </DETAILS>
  </TD></TR>
</TABLE>
{{{refresh}}}
)HTML";

static const char pageUpdateBusy[] PROGMEM = R"HTML(<SCRIPT>setTimeout(()=>location.replace('/update'),1000);</SCRIPT>)HTML";

static const char pageUpdateComplete[] PROGMEM = R"HTML(<SCRIPT>setTimeout(()=>location.replace('/'),20000);</SCRIPT>)HTML";

#endif // PAGE_UPDATE_H
