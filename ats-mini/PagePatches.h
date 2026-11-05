#ifndef PAGE_PATCHES_H
#define PAGE_PATCHES_H

#include <Arduino.h>

static const char pagePatchesTitle[] PROGMEM = "ATS-Mini SI4732 补丁（实验性）";

static const char pagePatches[] PROGMEM = R"HTML(<H1>{{title}}</H1>
{{{navigation}}}
{{{busy}}}
<FORM METHOD="POST" ACTION="/patches">
  <INPUT TYPE="HIDDEN" NAME="action" VALUE="select">
  <TABLE COLUMNS="2">
    <TR><TH COLSPAN="2" CLASS="HEADING">当前补丁组</TH></TR>
    <TR><TD CLASS="LABEL"><LABEL FOR="patchslot">补丁组</LABEL></TD><TD><SELECT ID="patchslot" NAME="slot" {{disabled}}>{{{options}}}</SELECT></TD></TR>
    <TR><TH COLSPAN="2" CLASS="HEADING"><INPUT TYPE="SUBMIT" VALUE="应用" {{disabled}}></TH></TR>
  </TABLE>
</FORM>
{{{sets}}}
)HTML";

static const char pagePatchesBusy[] PROGMEM = R"HTML(<TABLE COLUMNS="1"><TR><TD CLASS="CENTER">补丁请求正在处理中。</TD></TR></TABLE>)HTML";

static const char pagePatchSet[] PROGMEM = R"HTML(<FORM METHOD="POST" ACTION="/patches" ENCTYPE="multipart/form-data">
  <!-- Slot and action must precede the file for upload callbacks. -->
  <INPUT TYPE="HIDDEN" NAME="slot" VALUE="{{slot}}">
  <INPUT TYPE="HIDDEN" NAME="action" VALUE="upload">
  <TABLE COLUMNS="2">
    <TR><TH COLSPAN="2" CLASS="HEADING">{{name}}</TH></TR>
    <TR><TD CLASS="LABEL">已安装补丁</TD><TD>{{installed}}</TD></TR>
    <TR><TD CLASS="LABEL"><LABEL FOR="patchmode{{slot}}">模式</LABEL></TD><TD><SELECT ID="patchmode{{slot}}" NAME="mode" {{disabled}}>{{{options}}}</SELECT></TD></TR>
    <TR>
      <TD CLASS="LABEL"><LABEL FOR="patchfile{{slot}}">上传补丁</LABEL></TD>
      <TD><INPUT TYPE="FILE" ID="patchfile{{slot}}" NAME="patch" ACCEPT=".bin" {{disabled}}><BR><SMALL>每次一个 .bin 文件；最大 32 KiB</SMALL></TD>
    </TR>
    {{{delete}}}
    <TR><TH COLSPAN="2" CLASS="HEADING"><INPUT TYPE="SUBMIT" VALUE="保存" {{disabled}}></TH></TR>
  </TABLE>
</FORM>
)HTML";

static const char pagePatchDelete[] PROGMEM = R"HTML(<TR><TD CLASS="LABEL"><LABEL FOR="patchdelete{{slot}}">删除补丁组</LABEL></TD><TD><INPUT TYPE="CHECKBOX" ID="patchdelete{{slot}}" NAME="delete" VALUE="on" {{disabled}}></TD></TR>)HTML";

static const char pagePatchOption[] PROGMEM = R"HTML(<OPTION VALUE="{{value}}" {{selected}}>{{label}}</OPTION>)HTML";

#endif // PAGE_PATCHES_H
