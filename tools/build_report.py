# -*- coding: utf-8 -*-
"""生成《家庭室内多节点烟雾报警系统 设计说明书》Word 文档。
用法: python tools/build_report.py
输出: docs/家庭室内多节点烟雾报警系统_设计说明书.docx
"""
import os
from docx import Document
from docx.shared import Pt, Cm, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "家庭室内多节点烟雾报警系统_设计说明书.docx")

doc = Document()

for s in doc.sections:
    s.page_width, s.page_height = Cm(21.0), Cm(29.7)
    s.left_margin = s.right_margin = Cm(2.6)
    s.top_margin = s.bottom_margin = Cm(2.5)

def cjk(style, ascii_font, ea_font, size=None, bold=None, color=None):
    style.font.name = ascii_font
    rPr = style.element.get_or_add_rPr()
    rf = rPr.find(qn('w:rFonts'))
    if rf is None:
        rf = OxmlElement('w:rFonts'); rPr.append(rf)
    rf.set(qn('w:ascii'), ascii_font); rf.set(qn('w:hAnsi'), ascii_font)
    rf.set(qn('w:eastAsia'), ea_font)
    if size is not None: style.font.size = Pt(size)
    if bold is not None: style.font.bold = bold
    if color is not None: style.font.color.rgb = RGBColor.from_string(color)

cjk(doc.styles['Normal'], 'Times New Roman', '宋体', 10.5)
for lvl, (sz, col) in enumerate([(18, '0B4F6C'), (15, '0B4F6C'), (13, '12506B'), (11.5, '12506B')], start=1):
    cjk(doc.styles['Heading %d' % lvl], 'Arial', '微软雅黑', sz, True, col)
cjk(doc.styles['Title'], 'Arial', '微软雅黑', 26, True, '08313F')

def P(text='', bold=False, italic=False, align=None, size=None, style=None, space_after=6):
    p = doc.add_paragraph(style=style)
    r = p.add_run(text)
    r.bold = bold; r.italic = italic
    if size: r.font.size = Pt(size)
    if align is not None: p.alignment = align
    p.paragraph_format.space_after = Pt(space_after)
    return p

def H(text, level):
    return doc.add_heading(text, level=level)

def BULLET(text, bold_prefix=None):
    p = doc.add_paragraph(style='List Bullet')
    if bold_prefix:
        r = p.add_run(bold_prefix); r.bold = True
    p.add_run(text)
    p.paragraph_format.space_after = Pt(3)
    return p

def TABLE(headers, rows, widths=None, header_bg='DCEBF5'):
    t = doc.add_table(rows=1, cols=len(headers))
    t.style = 'Table Grid'
    t.alignment = WD_TABLE_ALIGNMENT.CENTER
    hdr = t.rows[0].cells
    for i, htxt in enumerate(headers):
        hdr[i].text = ''
        run = hdr[i].paragraphs[0].add_run(htxt); run.bold = True; run.font.size = Pt(9.5)
        shd = OxmlElement('w:shd'); shd.set(qn('w:fill'), header_bg)
        hdr[i]._tc.get_or_add_tcPr().append(shd)
    for row in rows:
        cells = t.add_row().cells
        for i, val in enumerate(row):
            cells[i].text = ''
            run = cells[i].paragraphs[0].add_run(str(val)); run.font.size = Pt(9.5)
    if widths:
        t.autofit = False          # python-docx 会正确写入 w:tblLayout type="fixed"
        tblPr = t._tbl.tblPr
        # 注意：必须"原地修改"已有的 tblW。OOXML 对 tblPr 子元素顺序有要求，
        # 直接 append 一条新的 tblW（落在 tblLook 之后）会被 Word/LibreOffice 忽略，
        # 表格就仍然是 auto 宽度、被长文本撑出版心。
        tblW = tblPr.find(qn('w:tblW'))
        if tblW is None:
            tblW = OxmlElement('w:tblW'); tblPr.append(tblW)
        tblW.set(qn('w:type'), 'dxa')
        tblW.set(qn('w:w'), str(int(sum(widths) * 567)))
        for i, w in enumerate(widths):
            t.columns[i].width = Cm(w)
        for r_ in t.rows:
            for i, w in enumerate(widths):
                r_.cells[i].width = Cm(w)
    doc.add_paragraph().paragraph_format.space_after = Pt(2)
    return t

def CODE(lines):
    p = doc.add_paragraph()
    p.paragraph_format.space_before = Pt(4); p.paragraph_format.space_after = Pt(8)
    p.paragraph_format.left_indent = Cm(0.5)
    for i, ln in enumerate(lines):
        r = p.add_run(ln + ('\n' if i < len(lines) - 1 else ''))
        r.font.name = 'Consolas'; r.font.size = Pt(9)
        r._element.rPr.rFonts.set(qn('w:eastAsia'), 'Consolas')
    return p

def PAGE_FOOTER():
    for s in doc.sections:
        p = s.footer.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        run = p.add_run()
        f1 = OxmlElement('w:fldChar'); f1.set(qn('w:fldCharType'), 'begin')
        it = OxmlElement('w:instrText'); it.set(qn('xml:space'), 'preserve'); it.text = 'PAGE'
        f2 = OxmlElement('w:fldChar'); f2.set(qn('w:fldCharType'), 'end')
        run._r.append(f1); run._r.append(it); run._r.append(f2)
        run.font.size = Pt(9)

# ---------------- 封面 ----------------
for _ in range(4):
    doc.add_paragraph()
t = doc.add_paragraph(style='Title'); t.alignment = WD_ALIGN_PARAGRAPH.CENTER
t.add_run('家庭室内多节点烟雾报警系统')
P('设 计 说 明 书', bold=True, align=WD_ALIGN_PARAGRAPH.CENTER, size=16, space_after=18)
P('三路 CH573F 蓝牙气敏节点 · 双路线 ESP32 网关 · 云端监测大屏',
  align=WD_ALIGN_PARAGRAPH.CENTER, size=11.5, italic=True, space_after=36)
TABLE(['项目', '内容'],
      [['项目名称', '家庭室内多节点烟雾报警系统'],
       ['系统构成', 'CH573F 节点固件、ESP32 网关固件（无外设版 / 触摸屏版）、云端服务与网页'],
       ['开发技术', 'C（RISC-V / ESP-IDF）、Python 3、JavaScript'],
       ['文档版本', '1.0'],
       ['作者 / 单位', '______________________'],
       ['日期', '________ 年 ____ 月 ____ 日']],
      widths=[3.2, 12.4])
doc.add_page_break()

# ---------------- 目录 ----------------
H('目 录', 1)
for a, b in [
    ('1  项目概述', '1.1 项目背景    1.2 设计目标    1.3 系统组成与成果'),
    ('2  系统总体设计', '2.1 总体架构    2.2 数据流    2.3 组网方式选型'),
    ('3  硬件设计', '3.1 硬件清单    3.2 节点接线    3.3 传感器选型与数据含义    3.4 两条网关路线'),
    ('4  节点固件设计', '4.1 软件结构    4.2 电阻换算    4.3 自适应基线与报警判据    4.4 上报与命令    4.5 配置项'),
    ('5  网关设计', '5.1 公共设计    5.2 网关A（无外设版）    5.3 网关B（触摸屏版）    5.4 差异对照    5.5 单射频稳定性    5.6 崩溃定位'),
    ('6  通信协议设计', '6.1 BLE 帧格式    6.2 MQTT 主题    6.3 HTTP/SSE 接口    6.4 安全设计'),
    ('7  云端与网页设计', '7.1 服务端结构    7.2 网页功能    7.3 性能优化'),
    ('8  系统测试与结果', '8.1 节点测试    8.2 网关测试    8.3 云端与网页测试    8.4 实测数据汇总'),
    ('9  关键技术问题与解决', '问题、现象、原因与处理对照表'),
    ('10 总结与展望', '10.1 已完成工作    10.2 不足与改进方向'),
    ('附录', 'A 引脚定义表    B 主要配置项    C 源码与文档索引'),
]:
    p = doc.add_paragraph(); p.paragraph_format.space_after = Pt(4)
    r = p.add_run(a); r.bold = True
    p.add_run('　' + b).font.size = Pt(9.5)
doc.add_page_break()

# ---------------- 1 ----------------
H('1  项目概述', 1)
H('1.1 项目背景', 2)
P('家庭燃气泄漏与火灾初期的烟雾，往往在人员察觉之前就已达到危险浓度。市售烟雾报警器多为单点独立设备：'
  '报警只在本地响，家人外出时无法得知，也没有历史数据可回溯。另一方面，若把气体传感器直接连到云端，'
  '又存在“断网即失效”的致命缺陷——而真正需要报警的时刻，恰恰可能是网络不可靠的时刻。')
P('本项目的出发点是把两者结合：判定放在本地、展示放到云端。节点在本地完成采样、基线与报警判定，'
  '断网、断服务器都照样响；同时把数据经网关送到云端，网页端可以查看实时数值、历史曲线与报警记录，'
  '并可远程下发消音、自检、复位指令。')

H('1.2 设计目标', 2)
BULLET('报警判定在节点 MCU 内完成，不依赖网关、WiFi 或服务器。', '① 本地判警、断网可用：')
BULLET('每个房间一个节点，统一编号（如 N01 / 厨房），云端按节点分别展示。', '② 多点覆盖：')
BULLET('不只上报数据，还要能下发消音、自检、复位等指令。', '③ 双向通信：')
BULLET('既要有“纯转发”的低成本网关，也要有能脱机演示、本地大屏显示与声响的形态。', '④ 两种网关形态：')
BULLET('云端网页提供实时数值、曲线、报警统计与事件流。', '⑤ 数据可视化：')

H('1.3 系统组成与成果', 2)
P('系统由四部分组成，均已实际制作并跑通：')
TABLE(['部分', '内容', '规模'],
      [['① 节点固件', 'CH573F（RISC-V + BLE 5.0），MQ 气敏传感器 + 有源蜂鸣器', '3 块'],
       ['② 网关 A', 'ESP32-S3，无外设，纯 BLE 到 MQTT 转发', '4 个源文件'],
       ['③ 网关 B', 'ESP32-2432S028R（2.8 寸触摸屏），本地大屏 + 报警声 + 一键标定', '9 个新增文件，界面约 1500 行'],
       ['④ 云端与网页', 'Mosquitto + Python 后端（HTTP/SSE/SQLite）+ 原生 JS 网页（ECharts）', '后端约 1000 行']],
      widths=[2.4, 8.6, 4.2])
P('系统实测可稳定运行：节点每 2 秒上报一帧（报警时 0.5 秒），整条链路端到端延迟约 1 秒，'
  '长时间运行无丢帧、无崩溃。')

# ---------------- 2 ----------------
H('2  系统总体设计', 1)
H('2.1 总体架构', 2)
P('系统采用“感知层 — 汇聚层 — 平台层 — 应用层”四层结构：')
TABLE(['层级', '组成', '职责'],
      [['感知层', 'CH573F 节点 ×3', '采样气体传感器、换算电阻、维护基线、本地判警与蜂鸣'],
       ['汇聚层', 'ESP32 网关（A 或 B）', 'BLE 主机：扫描/连接节点、校验解析帧；转 JSON 经 WiFi/MQTT 上云；下发云端指令'],
       ['平台层', 'Linux 服务器', 'Mosquitto Broker、Python 后端（REST + SSE）、SQLite 历史库'],
       ['应用层', '浏览器（手机/电脑）', '实时大屏、Δ% 曲线、报警统计、事件流、远程指令']],
      widths=[2.0, 4.4, 8.8])
P('节点与网关之间使用 BLE GATT；网关与云端之间使用 MQTT；网页与后端之间使用 HTTP + SSE。')

H('2.2 数据流', 2)
P('上行：节点每 2 秒（报警时 0.5 秒）通过 BLE Notify 发送 16 字节二进制遥测帧；网关校验、解析并转成 JSON，'
  '按主题 home/<网关>/tele/<MAC> 发布；后端落库后立即以 SSE 把带数值的消息推给网页，网页就地更新卡片与曲线。')
P('下行：网页点击“消音”后 POST /api/cmd；后端经 MQTT 发布到 home/<网关>/cmd/<MAC>；网关收到后通过 BLE '
  'Write 写入 0xFFE3；节点执行并回执，回执再经 MQTT 回到网页事件流。')

H('2.3 组网方式选型', 2)
TABLE(['方案', '能否满足需求', '结论'],
      [['BLE 主从（ESP32 做 Central）', '可同时连接多个节点；每条链路双向通信；CH573 协议栈本身就是外设角色', '采用'],
       ['BLE Mesh', '沁恒 CH57x 协议栈不提供 Mesh 模型（无 provisioning / relay / 元素模型），自行实现等于重写协议栈', '不采用'],
       ['纯广播（只广播不连接）', '广播包无链路层确认、丢包无法补救；且无法下发指令（消音/自检是刚需）', '不采用']],
      widths=[4.0, 8.6, 2.2])
P('结论：主从星型在可靠性（双向、有确认）与实现代价之间最优。')

# ---------------- 3 ----------------
H('3  硬件设计', 1)
H('3.1 硬件清单', 2)
TABLE(['序号', '部件', '型号 / 规格', '数量'],
      [['1', '节点主控', 'CH573F 最小系统板（RISC-V，BLE 5.0）', '3'],
       ['2', '气体传感器', 'MQ-2 / MQ-135 / MQ-137 模块', '各 1'],
       ['3', '蜂鸣器', '3.3V 有源蜂鸣器', '3'],
       ['4', '网关 A', 'ESP32-S3 开发板（无外设）', '1'],
       ['5', '网关 B', 'ESP32-2432S028R（2.8 寸 240×320 电阻触摸 + 功放）', '1'],
       ['6', '扬声器', '接板上功放输出（IO26）', '1'],
       ['7', '供电', '5V（MQ 模块加热丝额定 5V）', '—']],
      widths=[1.3, 3.0, 8.4, 1.5])

H('3.2 节点接线', 2)
TABLE(['信号', '引脚', '说明'],
      [['AO（模拟输出）', 'PA15（ADC 通道 AIN5）', '默认按 PA4 编译，可用编译参数 -AoPin 指定'],
       ['DO（数字输出）', 'PA5', '板上比较器输出，仅用于显示，不参与报警判定'],
       ['蜂鸣器', 'PB15', '有源蜂鸣器'],
       ['调试串口', 'PB4 / PB7', '115200，可用于打印实时数据']],
      widths=[3.4, 4.6, 7.2])
P('注意：CH573F 的 ADC 通道与引脚是固定绑定的（AIN0 对应 PA4、AIN1 对应 PA5、AIN5 对应 PA15……）。'
  '若 AO 实际接在 PA15 而固件仍按 PA4 采样，读到的就是悬空引脚的电源轨（约 3.3V 且数值恒定），'
  '表现为“数值不动”。工程提供 -AoPin 编译参数，可自动带出配套的 ADC 通道号。')

H('3.3 传感器选型与数据含义', 2)
TABLE(['型号', '主要检测对象', '判据类型', '数据含义'],
      [['MQ-2', '可燃气 / 烟雾（LPG、甲烷、丙烷、氢气、烟）', '安全类', '看 Δ%：负值越大表示电阻越小、浓度越高；|Δ%| ≥ 25% 触发报警'],
       ['MQ-135', '综合空气质量（NH₃、苯系物、烟等）', '环境类', '看 Rs/R0 指数：以清洁空气基线 R0 为参考，指数越高空气越好'],
       ['MQ-137', '氨气 NH₃', '环境类', '同 MQ-135']],
      widths=[2.0, 5.4, 1.9, 6.0])
P('两类判据分开设计的原因：安全类关心“现在比刚才差多少”，用自适应基线最灵敏；'
  '环境类关心“现在空气绝对好坏”，需要一个通过标定得到的绝对参考 R0。')

H('3.4 两条网关路线（硬件差异）', 2)
TABLE(['维度', '网关 A（无外设版）', '网关 B（触摸屏版）'],
      [['主控', 'ESP32-S3（双核，射频共存宽裕）', 'ESP32-WROOM-32E（经典 ESP32，单射频，4MB Flash）'],
       ['显示', '无', 'ILI9341 240×320（SPI2）'],
       ['输入', '无', 'XPT2046 电阻触摸（SPI3）'],
       ['声音', '无（仅节点蜂鸣器）', '板上功放 + 扬声器（IO26）'],
       ['其它外设', '无', 'RGB LED、光敏电阻、BOOT 键'],
       ['适用场景', '数据上云、二次开发', '脱机演示、答辩现场、本地运维']],
      widths=[2.4, 5.9, 6.9])

# ---------------- 4 ----------------
H('4  节点固件设计', 1)
H('4.1 软件结构', 2)
P('节点工程基于沁恒官方 SDK（MounRiver Studio 工程），应用代码集中在 APP 目录：')
BULLET('smoke_sensor.c：ADC 采样、电阻换算、基线维护、报警判定')
BULLET('smoke_node.c：主循环、上报调度、命令解析、蜂鸣器控制、信息帧组织')
BULLET('peripheral.c / gattprofile.c：BLE 外设初始化、广播、GATT 服务与通知')
BULLET('node_cfg.h：唯一需要按板子修改的配置头文件')

H('4.2 电阻换算', 2)
CODE(['Rs  = RL x (VCC - Vao) / Vao        // 传感器等效电阻',
      'D%  = (Rs / Rs0 - 1) x 100           // 相对自适应基线的变化率'])
P('其中 RL 为模块负载电阻，VCC 为模块供电电压。若模块接 5V，必须把 NODE_MODULE_VCC_MV 改为 5000；'
  '若 AO 经分压后再接 MCU，还需设置 NODE_AO_DIV_X100（10k/10k 分压填 200）。')

H('4.3 自适应基线与报警判据', 2)
BULLET('基线 Rs0 每样本最多跟随 1%，且只向“更干净”方向更新，避免把污染误当成新基线；')
BULLET('报警：|Δ%| ≥ 25% 连续 2 次；解除：|Δ%| < 12% 连续 5 次（迟滞设计，避免临界抖动反复报警）；')
BULLET('预热：上电后 30 秒内只上报不判警，帧内带 warmup 标志；')
BULLET('消音：M / M60 只停本机蜂鸣器，判定与上报照常进行。')

H('4.4 上报节奏与命令处理', 2)
TABLE(['状态', '采样周期', '上报周期'],
      [['正常', '500 ms', '2 s'],
       ['报警', '500 ms', '0.5 s'],
       ['预热（前 30 s）', '500 ms', '2 s，并置 warmup 标志']],
      widths=[3.6, 3.4, 8.2])
TABLE(['命令', '含义'],
      [['M / MUTE', '消音 60 秒（M 后跟数字即秒数，如 M30，上限 3600）'],
       ['T / TEST', '自检：蜂鸣器长鸣 3 秒'],
       ['R / RESET', '清除报警锁存（基线不动）'],
       ['其它字符串', '仅在节点串口日志打印一行']],
      widths=[3.6, 11.6])

H('4.5 关键配置项', 2)
TABLE(['宏定义', '默认值', '含义'],
      [['NODE_TAG', 'N01', '节点编号（显示用，建议每块不同）'],
       ['NODE_MODEL', 'MQ-2', '传感器型号（决定屏幕/网页上的数据语义）'],
       ['NODE_MODULE_VCC_MV', '3300', '模块供电电压（5V 供电改为 5000）'],
       ['NODE_AO_DIV_X100', '100', 'AO 分压比 ×100（10k/10k 分压填 200）'],
       ['NODE_RL_OHM', '1000', '模块负载电阻'],
       ['NODE_AO_CH / PIN / PIN_NAME', 'AIN0 / PA4', '受 #ifndef 保护，可用编译参数覆盖']],
      widths=[5.0, 2.4, 7.8])
P('为多块板出不同固件时无需修改源码，编译脚本支持直接覆盖参数，例如：')
CODE(['.\\tools\\build_node.ps1 -Tag N01 -Model "MQ-135" -AoPin PA15 -Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200 -Out .\\deploy\\fw\\N01_MQ-135.hex'])

# ---------------- 5 ----------------
H('5  网关设计', 1)
P('本项目的网关有两种实现，功能完全对等（都能跑通整套系统），区别只在是否带本地人机界面与外设。'
  '下文分别说明，并给出文件级差异对照，避免把两者混为一谈。')

H('5.1 公共设计（两条路线共有）', 2)
BULLET('BLE 主机：扫描并按设备名 SMOKE_NODE 过滤（不按服务 UUID 兜底，避免误连同类开发板）；')
BULLET('多连接管理：按 MAC 建立槽位，保存节点编号、型号、版本、最近数据与在线状态；')
BULLET('帧校验：校验 magic、类型与校验和，丢弃异常帧；')
BULLET('数据上云：转 JSON 后按主题发布，并用 SNTP 对时保证时间戳有效；')
BULLET('离线缓存：断网期间最多缓存 32 条，恢复后补发；')
BULLET('心跳与遗嘱：周期发布网关心跳；注册 LWT，异常掉电时由 Broker 代发离线消息。')

H('5.2 网关 A：ESP32-S3 无外设版', 2)
P('定位是“基线版本”，代码只有 4 个源文件，协议定义以它为准。')
TABLE(['文件', '职责'],
      [['gateway_main.c', 'BLE 主机流程、节点槽位管理、命令下发、状态打印'],
       ['wifi_mqtt.c / .h', 'WiFi 连接、MQTT 客户端、主题组装、离线缓存、SNTP 对时'],
       ['node_proto.h', 'BLE 帧格式与校验（与节点侧必须一致）'],
       ['app_config.h', '唯一配置文件：WiFi、MQTT 地址与口令、组网规模与节奏']],
      widths=[4.4, 10.8])

H('5.3 网关 B：CYD 触摸屏版', 2)
P('在 A 线基础上移植到 ESP32-2432S028R（经典 ESP32 + 2.8 寸触摸屏），并增加完整的本地交互。'
  '除 A 线的 4 个文件外，新增 9 个文件：')
TABLE(['文件', '职责'],
      [['board.c / board.h / board_config.h', '板级引脚定义与初始化'],
       ['display.c / display.h', 'LCD + 触摸 + LVGL 移植层（双 24 行缓冲）'],
       ['ui.c / ui.h', '全部界面：四个页签、曲线详情页、开机自检页、报警弹窗（约 1500 行）'],
       ['gw_buzzer.c / .h', 'LEDC 蜂鸣器（2.7 kHz，嘀嘀嘀-停 节奏）'],
       ['gw_cal.c / .h', '清洁空气标定值存储（NVS，按节点 MAC 索引）'],
       ['gw_hist.c / .h、gw_log.c / .h、gw_view.h', '事件环形缓存、串口日志、视图数据结构']],
      widths=[5.6, 9.6])
P('本地界面包含四个页签：监控（节点卡片：等级徽章、大号指数、Δ% 色条、趋势箭头、ADC/AO 电压/Rs/信号）、'
  '记录（事件时间线）、控制（消音、自检、复位与一键标定）、设置（背光亮度、夜间自动降背光等）；'
  '另有开机自检页与报警自动弹窗。')

H('5.4 两条路线的文件级差异', 2)
TABLE(['文件', '网关 A', '网关 B', '说明'],
      [['node_proto.h', '有', '有（逐字节相同）', '协议定义，两侧必须一致'],
       ['wifi_mqtt.c / .h', '有', '基本相同', 'B 线额外提供获取 IP 与信号强度的接口给屏幕使用'],
       ['app_config.h', '有', '有差异', 'B 线增加探头自检阈值等配置'],
       ['gateway_main.c', '基线', '多处标记改动', '单射频调度、LVGL 任务、标定、NVS 等'],
       ['board / display / ui / gw_*', '无', 'B 线独有', '外设与本地界面全部在这 9 个文件里']],
      widths=[4.4, 1.8, 4.0, 5.0])

H('5.5 关键技术：经典 ESP32 单射频下的 BLE 连接稳定性', 2)
P('ESP32-S3 为双核，BLE 与 WiFi 共存宽裕；而经典 ESP32 只有一个射频，BLE 扫描/连接会与 WiFi 抢占时隙。'
  '把 A 线代码直接烧进 CYD 后，扫描能发现节点，但连接反复失败：')
CODE(['GW: 发现节点 AA:BB:CC:DD:EE:01 (类型 0) -> 分配为 节点1, 正在连接...',
      'GW: | 节点1  连接中 AA:BB:CC:DD:EE:01  RSSI=0 dBm (等数据)   <- 卡住 20 秒以上',
      'W: BT_L2CAP: L2CA_CancelBleConnectReq - no connection pending',
      'W: x 节点1 (AA:BB:CC:DD:EE:01) 连接断开, 原因 0x00          <- 然后无限重试'])
P('原因：原工程在扫描回调里直接发起连接，同时保持扫描不停（扫描占空比约 50%），单射频下连接过程被扫描打断。')
P('解决：① 只在启动阶段扫描，发现节点后停止扫描，仅维护已有连接；② 扫描、MQTT 重连与 LVGL 刷新错峰进行；'
  '③ 连接参数更新（每 4 秒一次）改到 MQTT 空闲窗口再发送；④ 报警期间节点上报密度提高到 0.5 秒每帧，'
  '这是单射频下最紧张的时刻，程序在此处做了额外节流。')

H('5.6 关键技术：临界区中调用时间函数导致的崩溃', 2)
P('移植后在报警高负载下出现偶发崩溃（看门狗复位）。定位发现：时钟格式化函数在持有临界区'
  '（portENTER_CRITICAL）期间调用了 localtime_r()，而该函数内部会加锁并可能触发调度，'
  '在临界区内调用属于非法操作。')
P('解决：把时钟格式化移出临界区，改为在临界区外预先格式化并缓存字符串，临界区内只做赋值。'
  '同时加固了两处真实风险：LVGL 刷屏任务与 BLE 回调的共享数据加保护、MQTT 发布前的字符串拼接改为定长缓冲。')

# ---------------- 6 ----------------
H('6  通信协议设计', 1)
H('6.1 BLE GATT 与帧格式', 2)
P('节点为外设角色（广播名 SMOKE_NODE），网关为主机角色。服务沿用沁恒 SimpleProfile（0xFFE0）：')
TABLE(['特征', 'UUID', '方向', '用途'],
      [['CHAR3', '0xFFE3', '网关到节点（Write）', '下发命令（ASCII，不超过 20 字节）'],
       ['CHAR4', '0xFFE4', '节点到网关（Notify）', '遥测帧 / 信息帧']],
      widths=[2.2, 2.4, 4.8, 5.8])
P('因默认 MTU 为 23（单次负载仅 20 字节），遥测采用二进制帧而非 JSON。遥测帧共 16 字节（小端）：')
TABLE(['偏移', '长度', '字段', '说明'],
      [['0', '1', 'magic', '固定 0x53（字符 S）'],
       ['1', '1', 'type', '0x01 遥测'],
       ['2', '1', 'seq', '序号，用于粗查丢包'],
       ['3', '1', 'flags', 'bit0 报警、bit1 预热、bit2 DO 为高、bit3 已消音'],
       ['4', '2', 'adc', 'ADC 原始值 0~4095'],
       ['6', '2', 'ao_mv', '模块 AO 电压（mV）'],
       ['8', '4', 'rs', '等效电阻 Rs（Ω）'],
       ['12', '2', 'dpct', 'Δ% ×10，有符号（-523 表示 -52.3%）'],
       ['14', '1', 'do', 'DO 电平'],
       ['15', '1', 'sum', '前 15 字节求和取低 8 位']],
      widths=[1.6, 1.6, 2.6, 9.4])
P('信息帧：0x53 + 0x02 + 18 字节 ASCII「编号|型号|版本」（如 N01|MQ-2|1.0），连接建立后发送一次，之后每 30 秒补发。')

H('6.2 MQTT 主题与报文', 2)
TABLE(['主题', '方向', 'retain', '说明'],
      [['home/gw01/tele/<MAC>', '上行', '否', '遥测数据（约 2 秒一条）'],
       ['home/gw01/event/<MAC>', '上行', '否', '报警 / 解除事件'],
       ['home/gw01/state/<MAC>', '上行', '是', '节点在线状态 + 编号/型号/版本'],
       ['home/gw01/state/gateway', '上行', '是', '网关心跳（IP / 信号 / 在线节点数）'],
       ['home/gw01/ack/<MAC>', '上行', '否', '指令回执'],
       ['home/gw01/cmd/<MAC>', '下行', '否', '控制指令（纯文本）']],
      widths=[5.2, 1.8, 1.6, 6.6])
P('状态类消息使用 retain，网页随时打开都能立刻知道节点是否在线；网关注册遗嘱消息，异常掉电时由 Broker '
  '立即代发离线状态。遥测报文示例：')
CODE(['{"ts":1790933403,"mac":"AABBCCDDEEFF","seq":12,"adc":1035,"ao":1668,',
      ' "rs":1998,"dpct":-52.3,"alarm":1,"warmup":0,"do":0,"muted":0,"rssi":-45}'])

H('6.3 HTTP / SSE 接口', 2)
TABLE(['方法', '路径', '说明'],
      [['GET', '/api/status', '网关状态与全部节点当前值'],
       ['GET', '/api/series', '曲线数据；支持 hours、since（增量）、max_points（抽稀）、fmt=2（紧凑编码）'],
       ['GET', '/api/events', '事件流'],
       ['GET', '/api/stats', '每小时报警次数统计'],
       ['GET', '/api/stream', 'SSE 实时推送（tele / alarm / state）'],
       ['POST', '/api/cmd', '下发指令 {"mac":"…","text":"M"}']],
      widths=[1.8, 3.6, 9.8])

H('6.4 安全设计', 2)
BULLET('所有接口需要请求头 X-Auth: <访问口令>，SSE 使用 ?token=；', '· 接口鉴权：')
BULLET('设备账号只能写自己的上报主题、只读命令主题；后端账号可读全部、只能写命令主题；', '· MQTT ACL 最小权限：')
BULLET('推荐通过隧道以 HTTPS 暴露，避免直接暴露明文端口；', '· 外网访问：')
BULLET('仓库中的口令、域名、隧道 token 一律以占位符提交，真实配置单独保存且不纳入版本控制。', '· 仓库安全：')

# ---------------- 7 ----------------
H('7  云端与网页设计', 1)
H('7.1 服务端结构', 2)
TABLE(['模块', '技术', '说明'],
      [['MQTT Broker', 'Mosquitto 2.x', 'ACL 由安装脚本按配置自动生成'],
       ['后端', 'Python 3 + 标准库 HTTP 服务 + paho-mqtt + SQLite', '无 Web 框架依赖，单文件实现 REST 与 SSE'],
       ['数据库', 'SQLite', '设备表、遥测表（索引 ts / mac）、事件表；遥测默认保留 7 天'],
       ['部署', 'systemd 单元 + 一键安装脚本', '幂等，覆盖代码但不动数据库'],
       ['外网', '可选隧道（出站连接）', '无需备案域名即可 HTTPS 访问']],
      widths=[2.6, 5.4, 7.2])

H('7.2 网页功能', 2)
BULLET('在线节点数、当前状态、网关 IP、WiFi 信号、累计报警；', '· 顶部指标：')
BULLET('当前 Δ%、ADC、AO 电压、Rs、信号强度与最近报警；', '· 节点卡片：')
BULLET('可选 10 分钟至 24 小时窗口，红点标记报警时刻，虚线为 ±25% 阈值；', '· 曲线：')
BULLET('近 24 小时按小时堆叠的柱状图；', '· 报警统计：')
BULLET('报警、解除、上下线、指令与回执；', '· 事件流：')
BULLET('消音、自检、复位与自定义文本。', '· 指令下发：')

H('7.3 性能优化', 2)
P('网页端最初存在“点击切换曲线明显卡顿”的问题。定位发现瓶颈不在渲染，而在请求模型：'
  '前端每收到一条遥测推送就调用一次状态接口，多节点时形成请求风暴；公网隧道单次请求约 1.5 秒，'
  '请求排队导致界面响应迟滞。改进如下：')
TABLE(['优化项', '效果'],
      [['后端 gzip 压缩', '网页首屏 1.16 MB 降至 383 KB'],
       ['曲线点紧凑编码（数组代替对象）', '1359 个点 80.4 KB 降至 30.1 KB（减少约 63%）'],
       ['SSE 遥测消息直接携带数值', '前端更新卡片不再发起 HTTP 请求，请求量下降约两个数量级'],
       ['曲线按（节点, 时间范围）缓存并与空闲时预热', '切换节点有缓存时零请求、瞬时出图'],
       ['卡片 DOM 就地更新、选择器按签名重建', '按钮不再被反复销毁重建，点击不再“落空”'],
       ['图表采样并避免整图重建', '切换曲线不再整块重画'],
       ['隧道改为单条 http2 长连接', '空闲流量降至约 12 MB/天']],
      widths=[6.6, 8.6])

# ---------------- 8 ----------------
H('8  系统测试与结果', 1)
H('8.1 节点测试', 2)
TABLE(['测试项', '结果'],
      [['上电与广播', '上电后 1 秒内开始广播，网关可扫描发现'],
       ['预热机制', '前 30 秒只上报不判警，warmup 标志正确置位'],
       ['报警响应', '丁烷靠近 2~3 秒后 Δ% 快速下降，超过 25% 连续 2 次即报警并驱动蜂鸣器'],
       ['报警解除', '撤去气源后约 1 分钟自动解除（低于 12% 连续 5 次）'],
       ['命令响应', '消音、自检、复位均可正确执行并返回回执'],
       ['断网可用性', '关闭网关后节点仍能本地报警，蜂鸣器正常']],
      widths=[3.4, 11.8])

H('8.2 网关测试', 2)
TABLE(['测试项', '结果'],
      [['多节点连接', '同时连接 3 个节点，槽位状态均显示“已就绪”'],
       ['帧校验', '注入错误校验和的帧被正确丢弃，串口有告警日志'],
       ['离线缓存', '断网 1 分钟内的数据在恢复后补发，未丢失'],
       ['单射频稳定性（B 线）', '连续运行数小时无断连；报警高负载期间无复位'],
       ['空闲内存（B 线）', '连接 3 个节点稳定在 30 KB 以上，历史最低约 24 KB'],
       ['固件体积（B 线）', '应用约 2.19 MB，3 MB 分区剩余约 30%']],
      widths=[3.4, 11.8])

H('8.3 云端与网页测试', 2)
TABLE(['测试项', '结果'],
      [['端到端延迟', '节点采样到网页显示约 1 秒'],
       ['曲线连续性', '序号连续、无丢点；报警跳变时曲线有明显阶跃'],
       ['曲线接口性能', '1359 点约 30 KB；增量请求约 170 字节'],
       ['并发访问', '手机与电脑同时打开，数据一致'],
       ['移动网络访问', '通过隧道 HTTPS 可在手机流量下正常打开']],
      widths=[3.4, 11.8])

H('8.4 实测数据汇总', 2)
TABLE(['指标', '数值'],
      [['节点上报周期', '正常 2 s / 报警 0.5 s；单帧 16 字节'],
       ['采样周期', '500 ms'],
       ['报警 / 解除阈值', '|Δ%| ≥ 25%（连续 2 次）/ 低于 12%（连续 5 次）'],
       ['预热时间', '30 s'],
       ['网关多连接数', '默认 4（可配置）'],
       ['网页首屏（gzip 后）', '383 KB'],
       ['曲线数据量', '1359 点约 30 KB；增量约 170 字节'],
       ['隧道空闲流量', '约 12 MB/天']],
      widths=[5.2, 10.0])

# ---------------- 9 ----------------
H('9  关键技术问题与解决', 1)
TABLE(['问题', '现象', '原因与解决'],
      [['经典 ESP32 单射频', '报警期间反复断连', '扫描与连接/WiFi 抢占时隙；改为只在启动阶段扫描，各部分错峰'],
       ['临界区内调用时间函数', '高负载下偶发看门狗复位', '函数内部加锁；时钟格式化移出临界区并缓存结果'],
       ['ADC 通道与引脚绑定', '读数恒定贴在电源轨', 'AO 接 PA15 而固件按 PA4 采样；提供编译参数自动带出通道'],
       ['探头未接被判为“正常”', '悬空引脚读数被基线拉到 0，显示一切正常', '悬空值贴轨且不抖动；增加滑动窗口探头自检，显式报“故障”'],
       ['探头自检“假恢复”', '单个计数抖动即打印“恢复正常”', '判据改为窗口内极差不超过 1，不再被单点抖动欺骗'],
       ['中文路径 + ESP-IDF', 'UnicodeDecodeError: gbk codec', '工程放纯英文路径并预设 UTF-8 环境变量'],
       ['PowerShell 参数引号', '宏定义被当作未声明标识符', '以转义引号形式传递，保证进入编译器的字符串带引号'],
       ['脚本编码', '中文注释导致解析错误', '脚本统一保存为带 BOM 的 UTF-8'],
       ['CDN 缓存导致界面不一致', '域名访问为旧界面、IP 直连为新界面', '静态资源加版本号并设置合理的缓存策略'],
       ['隧道流量偏高', '空闲也消耗数十至数百 MB/天', '改为单条 http2 长连接，实测降至约 12 MB/天']],
      widths=[3.4, 4.4, 7.4])

# ---------------- 10 ----------------
H('10  总结与展望', 1)
H('10.1 已完成工作', 2)
BULLET('完成三块 CH573F 节点固件，实现本地采样、自适应基线、迟滞报警、蜂鸣器与命令处理；')
BULLET('完成两种 ESP32 网关（无外设版与触摸屏版）固件，实现多节点 BLE 连接、帧校验、云端转发与指令下发；')
BULLET('完成触摸屏版本地交互：四页签界面、曲线详情、清洁空气标定、探头自检、报警弹窗与报警声；')
BULLET('完成云端服务（Mosquitto + 后端 + SQLite）与网页大屏（实时数值、曲线、统计、事件流、远程指令）；')
BULLET('完成网页性能优化：请求量下降约两个数量级，曲线数据量减少约 63%，切换曲线无卡顿。')

H('10.2 不足与改进方向', 2)
TABLE(['方面', '现状', '改进方向'],
      [['传感器精度', 'MQ 系列对温湿度敏感，绝对精度有限', '增加温湿度补偿；引入更高精度传感器'],
       ['标定方式', '环境类需在清洁空气下人工标定', '标定值上云同步，换网关自动恢复'],
       ['事件持久化', '事件为内存环形缓存，掉电即失', '重要事件写入 Flash 或直接上云保存'],
       ['单射频限制', '带屏版可连接节点数量受限', '改用 ESP32-C6 或 ESP32-S3 等支持并发的型号'],
       ['节点维护', '更改基线需重新上电等待预热', '新增重取基线命令，一键完成'],
       ['开放能力', '暂无第三方接入接口', '开放 Webhook 与数据导出接口，便于接入智能家居平台']],
      widths=[2.6, 5.6, 7.0])

# ---------------- 附录 ----------------
doc.add_page_break()
H('附录 A  引脚定义表', 1)
TABLE(['部位', '信号', '引脚'],
      [['CH573F 节点', 'AO（模拟输出）', 'PA15（ADC 通道 AIN5，可用 -AoPin 修改）'],
       ['CH573F 节点', 'DO（数字输出）', 'PA5'],
       ['CH573F 节点', '蜂鸣器', 'PB15'],
       ['CH573F 节点', '调试串口', 'PB4 / PB7'],
       ['CYD 网关 · LCD', 'SPI2', 'SCLK 14、MOSI 13、MISO 12、CS 15、DC 2、BL 21'],
       ['CYD 网关 · 触摸', 'SPI3', 'SCLK 25、MOSI 32、MISO 39、CS 33、IRQ 36'],
       ['CYD 网关', '扬声器 / 功放', 'IO26'],
       ['CYD 网关', 'RGB LED', 'R 4、G 16、B 17'],
       ['CYD 网关', '其它', 'BOOT 键 IO0、光敏电阻 IO34']],
      widths=[3.4, 3.4, 8.4])

H('附录 B  主要配置项', 1)
TABLE(['位置', '配置项', '说明'],
      [['节点 node_cfg.h', 'NODE_TAG / NODE_MODEL', '节点编号与传感器型号'],
       ['节点 node_cfg.h', 'NODE_MODULE_VCC_MV / NODE_AO_DIV_X100 / NODE_RL_OHM', '供电电压、分压比、负载电阻'],
       ['节点 node_cfg.h', 'NODE_AO_CH / NODE_AO_PIN', 'AO 对应的 ADC 通道与引脚'],
       ['网关 app_config.h', 'APP_WIFI_SSID / APP_WIFI_PASSWORD', '2.4 GHz WiFi 账号与口令（占位符）'],
       ['网关 app_config.h', 'APP_MQTT_URI / USERNAME / PASSWORD', 'MQTT 服务器与账号（占位符）'],
       ['网关 app_config.h', 'APP_MAX_NODES / APP_HEARTBEAT_SEC', '最大连接节点数与心跳周期'],
       ['服务端 config.json', 'web / mqtt / limits', '网页口令、MQTT 账号、存储与超时限制']],
      widths=[3.4, 5.6, 6.2])

H('附录 C  源码与文档索引', 1)
TABLE(['路径', '内容'],
      [['README.md', '项目总览（仓库首页）'],
       ['docs/01 ~ 05', '系统架构、硬件接线、通信协议、编译烧写、答辩要点'],
       ['docs/06_运维与部署实操.md', '日常运维、部署与排障备忘'],
       ['node_ch573f/', '① 节点固件（MounRiver Studio 工程）'],
       ['gateway_esp32s3/', '② 网关 A：无外设版（ESP-IDF）'],
       ['cyd_gateway/', '③ 网关 B：带触摸屏外设版（ESP-IDF + LVGL）'],
       ['server/', '④ 云端：Broker 配置、后端、网页、部署脚本'],
       ['deploy/ 与 tools/', '打包、安装、流量诊断脚本与编译环境脚本']],
      widths=[6.0, 9.2])

PAGE_FOOTER()
os.makedirs(os.path.dirname(OUT), exist_ok=True)
doc.save(OUT)
print('已生成:', OUT)
print('段落数:', len(doc.paragraphs), '表格数:', len(doc.tables))
