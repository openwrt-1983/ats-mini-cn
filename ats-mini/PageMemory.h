#ifndef PAGE_MEMORY_H
#define PAGE_MEMORY_H

#include <Arduino.h>

static const char pageMemoryTitle[] PROGMEM = "ATS-Mini 记忆频道";

static const char pageMemory[] PROGMEM = R"HTML(<STYLE>
  TABLE.MEMORY { border-collapse: collapse; margin-bottom: 1em; }
  .MEMORY TH, .MEMORY TD { padding: 0.5em 0.125em; }
  .MEMORY THEAD TH { background-color: #80A0FF; text-align: left; font-size: 0.75em; }
  .MEMORY TBODY TR:nth-child(even) { background-color: #EDF2FF; }
  .MEMORY INPUT, .MEMORY SELECT { box-sizing: border-box; min-width: 0; width: auto; padding: 0.4em 0.1em; font: inherit; }
  .MEMORY INPUT { width: 100%; }
  .MEMORY INPUT[data-field=freq] { max-width: 12ch; text-align: right; font-variant-numeric: tabular-nums; }
  .MEMORY .MOVE { white-space: nowrap; }
  .MEMORY .MOVE BUTTON { display: inline-flex; align-items: center; justify-content: center; box-sizing: border-box; width: 26px; min-width: 0; min-height: 32px; padding: 0; font-size: 1em; }
  .MEMORY-NAV, #memory-status, .MEMORY-HELP { max-width: 768px; margin: 1em auto; padding: 0 0.5em; box-sizing: border-box; }
  .MEMORY-NAV { display: flex; gap: 1em; align-items: center; }
  .MEMORY-NAV BUTTON { min-width: 72px; min-height: 44px; padding: 0.5em 1em; font: inherit; }
  #memory-status, .MEMORY-STATE { font-size: 0.9em; }
  #memory-status:empty { display: none; }
  @media (max-width: 600px) {
    .MEMORY INPUT[data-field=name] { max-width: 10ch; }
  }
</STYLE>
<H1>{{title}}</H1>
{{{navigation}}}
<P CLASS="MEMORY-HELP">以 Hz 为单位编辑频率（0 表示清空该槽位）。名称最多 9 个可打印 ASCII 字符。
使用上/下按钮调整槽位顺序，按“保存”应用修改。</P>
<FORM ID="memories" METHOD="post" ACTION="/memory" AUTOCOMPLETE="on" ONINPUT="changed()">
  {{{toolbar}}}
  <P ID="memory-status" ROLE="status"></P>
  <INPUT TYPE="file" ID="memory-file" ACCEPT=".json,application/json" HIDDEN>
  <TABLE CLASS="MEMORY">
    <THEAD><TR>
      <TH SCOPE="col" ARIA-LABEL="Slot">#</TH><TH SCOPE="col">名称</TH><TH SCOPE="col">波段</TH>
      <TH SCOPE="col">频率 (Hz)</TH><TH SCOPE="col">模式</TH><TH SCOPE="col">移动</TH>
    </TR></THEAD>
    <TBODY>{{{rows}}}</TBODY>
  </TABLE>
  {{{toolbar}}}
</FORM>
<SCRIPT>
const form = document.getElementById('memories');
const body = form.querySelector('tbody');
const status = document.getElementById('memory-status');
const file = document.getElementById('memory-file');
const fields = ['band', 'freq', 'mode', 'name'];
const rows = Array.from(body.rows);
const cell = (row, field) => row.querySelector('[data-field="' + field + '"]');
const bandNames = Array.from(cell(rows[0], 'band').options, option => option.value);
const modeNames = Array.from(cell(rows[0], 'mode').options, option => option.value);
const read = () => rows.map(row => Object.fromEntries(fields.map(field =>
  [field, field === 'freq' ? Number(cell(row, field).value) : cell(row, field).value])));

function changed() {
  form.querySelectorAll('.MEMORY-STATE').forEach(label => label.textContent = '有未保存的修改');
  status.textContent = '';
}

function validateImport(data) {
  if(!Array.isArray(data) || data.length !== rows.length)
    throw Error('文件必须恰好包含 ' + rows.length + ' 个记忆槽位。');
  return data.map((m, i) => {
    const name = m?.name === undefined ? '' : m.name;
    if(!m || !bandNames.includes(m.band) ||
       !modeNames.includes(m.mode) ||
       typeof name !== 'string' || !/^[\x20-\x7e]{0,9}$/.test(name) ||
       !Number.isInteger(m.freq) || m.freq < 0 || m.freq > 4294967295)
      throw Error('槽位 ' + (i + 1) + '：记忆数据无效。');
    return {band: m.band, freq: m.freq, mode: m.mode, name};
  });
}

form.addEventListener('submit', () => {
  rows.forEach(row => cell(row, 'freq').value = Number(cell(row, 'freq').value));
});
function exportMemories() {
  if(!form.reportValidity()) return;
  try {
    const data = read();
    const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2) + '\n'], {type: 'application/json'}));
    const download = document.createElement('a');
    download.href = url;
    download.download = 'memories.json';
    download.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch(error) { status.textContent = error.message; }
}
file.addEventListener('change', async () => {
  if(!file.files.length) return;
  try {
    if(file.files[0].size > 65536) throw Error('记忆文件过大。');
    const data = validateImport(JSON.parse(await file.files[0].text()));
    rows.forEach((row, i) => fields.forEach(field => cell(row, field).value = data[i][field]));
    changed();
  } catch(error) { status.textContent = '导入失败：' + error.message; }
  file.value = '';
});

function moveRow(button, direction) {
  const row = button.closest('tr');
  const up = direction < 0;
  const target = up ? row.previousElementSibling : row.nextElementSibling;
  if(!target) return;
  fields.forEach(field => {
    const source = cell(row, field), destination = cell(target, field);
    [source.value, destination.value] = [destination.value, source.value];
  });
  changed();
  const next = target.querySelectorAll('button')[up ? 0 : 1];
  (next.disabled ? target.querySelector('button:not(:disabled)') : next).focus();
}
</SCRIPT>
)HTML";

static const char pageMemoryToolbar[] PROGMEM = R"HTML(<DIV CLASS="MEMORY-NAV">
    <A HREF="#" ONCLICK="exportMemories(); return false;">导出</A>
    <A HREF="#memory-file" ONCLICK="file.click(); return false;">导入</A>
    <BUTTON TYPE="submit">保存</BUTTON><SPAN CLASS="MEMORY-STATE" ROLE="status"></SPAN>
  </DIV>)HTML";

static const char pageMemoryRow[] PROGMEM = R"HTML(<TR>
  <TH SCOPE="row">{{slot}}</TH>
  <TD><INPUT DATA-FIELD="name" NAME="name{{index}}" VALUE="{{name}}" ARIA-LABEL="名称" SIZE="9" MAXLENGTH="9" PATTERN="[ -~]{0,9}"></TD>
  <TD><SELECT DATA-FIELD="band" NAME="band{{index}}" ARIA-LABEL="波段"><OPTION VALUE=""></OPTION>{{{bands}}}</SELECT></TD>
  <TD><INPUT DATA-FIELD="freq" NAME="freq{{index}}" VALUE="{{frequency}}" ARIA-LABEL="频率 (Hz)" TYPE="number" MIN="0" MAX="4294967295" REQUIRED></TD>
  <TD><SELECT DATA-FIELD="mode" NAME="mode{{index}}" ARIA-LABEL="模式"><OPTION VALUE=""></OPTION>{{{modes}}}</SELECT></TD>
  <TD CLASS="MOVE">
    <BUTTON TYPE="button" ONCLICK="moveRow(this, -1)" TITLE="上移" ARIA-LABEL="上移槽位 {{slot}}" {{up_disabled}}>&#8593;</BUTTON>
    <BUTTON TYPE="button" ONCLICK="moveRow(this, 1)" TITLE="下移" ARIA-LABEL="下移槽位 {{slot}}" {{down_disabled}}>&#8595;</BUTTON>
  </TD>
</TR>
)HTML";

static const char pageMemoryOption[] PROGMEM = R"HTML(<OPTION VALUE="{{value}}" {{selected}}>{{value}}</OPTION>)HTML";

static const char pageMemoryError[] PROGMEM = R"HTML(<H1>{{title}}</H1>
<P>Slot {{slot}}: {{error}} 未保存任何修改。请使用浏览器后退按钮返回修改表格。</P>)HTML";

#endif // PAGE_MEMORY_H
