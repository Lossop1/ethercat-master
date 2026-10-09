#!/usr/bin/env python3
"""闅忔椂闂村彉鍖栫殑鏇茬嚎銆?*鑷粯**锛屼笉鐢?QtChart鈥斺€旈姗欐淳涓婅涓嶅埌閭ｄ釜妯″潡锛堣涓嬶級銆?
**涓轰粈涔堟槸 QPainter 鑰屼笉鏄煇涓幇鎴愬浘琛ㄦ帶浠躲€?* 鍙版灦涓婅窇鐣岄潰鐨勬満鍣ㄦ槸棣欐娲撅紝
apt 甯︾殑鍙湁 PyQt5 鐨?QtWidgets 閭ｄ竴濂楋紝`QtChart` 涓嶅湪閲岄潰锛岃€屼笖閭ｅ彴鏈哄櫒娌℃湁缃戙€佽
涓嶄簡銆傛墍浠ユ洸绾垮彧鑳借嚜宸辩敾銆傝繖涓嶆槸"鎯宠嚜宸卞啓"锛屾槸娌℃湁鍒殑閫夋嫨鈥斺€旀墍浠ヨ繖閲屽敖閲忓皯鍋氫簨锛?鍙敾鎶樼嚎锛屼笉鍋氫氦浜掞紝涓嶅仛鍥句緥涔嬪鐨勮楗般€?
**鎶界█鍦ㄦ敹涓嬬殑鏃跺€欏仛锛屼笉鍦ㄧ敾鐨勬椂鍊欏仛銆?* 杩欐槸杩欎釜鏂囦欢閲屾渶閲嶈鐨勪竴鏉″喅瀹氥€?
涓荤珯鎸?1 kHz 鍑哄抚锛屼竴杞村洓鏉″簭鍒楋紙瀹為檯浣嶇疆/鐩爣/璺熼殢璇樊/鍔涚煩锛夈€傝鏄竴鑲¤剳鍏ㄥ瓨涓嬫潵锛?涓€鍒嗛挓 24 涓囦釜鐐癸紝鐢荤殑鏃跺€欐瘡閲嶇敾涓€娆″氨瑕佹壂涓€閬嶁€斺€?0 Hz 閲嶇敾灏辨槸姣忕浜旂櫨涓囨姣旇緝锛?瓒冲鍚冩帀棣欐娲剧殑涓€鏁翠釜鏍革紝鑰岄偅涓牳鏈潵璇ヨ窇涓荤珯锛堟湰杞殑瑕佹眰鏄?鍒拰涓荤珯鎶㈣祫婧?锛夈€?鎶婃娊绋€鎸埌鏀朵笅鐨勬椂鍊欙紝浠ｄ环灏变粠"姣忔閲嶇敾"鍙樻垚"姣忔潵涓€甯т竴娆?锛岃€屼笖鍙湁褰撳墠鏄剧ず鐨勯偅
鏍硅酱鍦ㄦ敹銆?
鎶界█鎵嬫硶鏄?*鎸夋椂闂存《鍙栨渶灏?鏈€澶?*锛堜笉鏄殧鍑犱釜鍙栦竴涓級锛?
  - 涓€妗剁暀涓や釜鏁帮紙鏈€灏忋€佹渶澶э級锛岀敾鐨勬椂鍊欑敾鎴愪竴鏉＄珫绾裤€?*鏋佸€奸兘鍦紝鍖呯粶灏变笉涓?*鈥斺€?    闅旂偣鎶芥牱浼氭妸灏栧嘲鏁翠釜婕忔帀锛岃€屽皷宄帮紙璺熼殢璇樊銆佸姏鐭╁啿鍑伙級鎭版伆鏄繖閲岃鐪嬬殑涓滆タ銆?  - 妗跺鎸夋樉绀鸿兘鍔涘畾锛氫竴涓閲庣暀 1000 涓《銆傚睆骞曚笂涓€灞忎篃灏变竴鍗冩潵鍒楋紝鍐嶅瀛樹篃鐢讳笉鍑烘潵銆?    60 绉掔殑绐?鈫?60 ms 涓€妗讹紝姣?1 kHz 鐨勫師濮嬬偣绮?60 鍊嶏紝鍐呭瓨鍜岄噸鐢诲紑閿€涓€璧烽檷 60 鍊嶃€?  - 浠ｄ环鍐欏湪鏄庡锛氫竴涓《鐨?*妗跺唴褰㈢姸**娌′簡锛堝彧鐭ラ亾涓婁笅鐣岋紝涓嶇煡閬撲腑闂存€庝箞璧扮殑锛夈€?    鐪嬭秼鍔裤€佺湅鏈夋病鏈夊皷宄版槸瀵圭殑鐢ㄩ€旓紱瑕佺湅鏌愪竴姣鐨勬尝褰紝閭ｄ笉褰掕繖涓晫闈㈢銆?
**鏃堕棿杞寸敤甯ц嚜甯︾殑 monotonic_ns**锛屼笉鏄湰鏈烘敹鍒扮殑鏃堕棿銆備富绔欐墍鏈夌殑甯ф潵鑷悓涓€涓椂閽燂紝
鐢ㄥ畠鎵嶈兘鎶?鍛戒护鍙戝嚭鍘荤殑杩欎竴鍒?鍜?杞村姩璧锋潵鐨勯偅涓€甯?瀵硅捣鏉ワ紱鐢ㄦ敹鍒扮殑鏈湴鏃堕棿浼氭妸
缃戠粶鎶栧姩绠楄繘鏇茬嚎锛岃€岄偅鎭版伆鏄繖閲岃鎺掗櫎鐨勪笢瑗裤€?"""

import array

from PyQt5.QtCore import QPoint, Qt
from PyQt5.QtGui import QColor, QFontMetrics, QPainter, QPen
from PyQt5.QtWidgets import QSizePolicy, QVBoxLayout, QWidget

# 鍥涙潯鏇茬嚎鐨勯鑹层€備綅缃拰鐩爣鏄悓涓€浠朵簨鐨勪袱闈紝鐢ㄨ繎浼艰壊锛涜宸拰鍔涚煩鍚勪竴鑹层€?COLOR_ACTUAL = QColor(0x1f, 0x77, 0xb4)
COLOR_TARGET = QColor(0xff, 0x7f, 0x0e)
COLOR_ERROR = QColor(0xd6, 0x27, 0x28)
COLOR_TORQUE = QColor(0x2c, 0xa0, 0x2c)
COLOR_AXIS = QColor(0x88, 0x88, 0x88)
COLOR_TEXT = QColor(0x33, 0x33, 0x33)
COLOR_BG = QColor(0xfa, 0xfa, 0xfa)

# 涓€杞翠笁鏉℃洸绾垮悇鍗犱竴涓í鏉★紙鍚勮嚜鏈夎嚜宸辩殑绾佃酱閲忕翰锛屾贩鍦ㄤ竴寮犲浘涓婃病娉曡锛夈€?PANE_ACTUAL = 0
PANE_ERROR = 1
PANE_TORQUE = 2
PANE_TITLES = ("浣嶇疆 / 鐩爣锛坈ounts锛?, "璺熼殢璇樊锛坈ounts锛?, "鍔涚煩锛堝師濮嬪€硷級")
PANE_HEIGHT_WEIGHT = (3, 2, 2)      # 浣嶇疆閭ｆ潯缁欓珮涓€鐐癸紝瀹冩槸涓昏鐪嬬殑
PANE_ORDER = (PANE_ACTUAL, PANE_ERROR, PANE_TORQUE)

LEFT_MARGIN = 92                    # 鐣欑粰绾佃酱鍒诲害鐨勫搴?RIGHT_MARGIN = 8
TOP_MARGIN = 4
BOTTOM_MARGIN = 22                  # 鐣欑粰鏃堕棿杞?MIN_PANE_H = 24

# 妯潯椤朵笂鐣欑粰鏍囬閭ｄ竴鏉＄殑楂樺害銆傛爣棰樺啓鍦?`top + 12`锛岀旱杞翠笂绔殑鍒诲害鍊煎啓鍦?# `to_y(hi) + 4`鈥斺€斾袱涓兘鍦?x=6锛屾爣棰樻病鍗犱綇鍦版柟鐨勬椂鍊欏埢搴﹀€间細鍘嬪埌鏍囬涓婏紙P12.18锛夈€?TITLE_H = 14
# 鏍囬涓嬮潰鑷冲皯瑕佺暀寰椾笅涓€娈垫洸绾匡紝涓嶇劧妯潯鍙槸涓爣棰樸€?MIN_PLOT_H = 16
# 鏇茬嚎鍖轰笂涓嬪悇璁╁嚭鏉ョ殑杈硅窛锛氭瀬鍊间笉璐磋竟妗嗙嚎锛屽埢搴﹀€间篃涓嶅嚭鐣屻€?HEADROOM = 7
# 涓婄鍒诲害鍊肩殑鍩虹嚎锛岀敾鍦ㄦ洸绾垮尯涓婅竟寰€涓嬭繖涔堜簺鍍忕礌銆?LABEL_DROP = 4

# 涓€寮犲浘鑷冲皯瑕佽繖涔堥珮銆備笁涓í鏉★紙浣嶇疆 3 浠姐€佽宸?2 浠姐€佸姏鐭?2 浠斤級鍚勮鐣欏緱涓嬪埢搴﹀拰
# 涓€鏉″儚鏍风殑鏇茬嚎锛屼綆浜庤繖涓暟灏卞彧鍓╀笁鏉＄粏缂濅簡銆傚鍣ㄧ殑鏈€灏忛珮搴︿篃鐢ㄥ畠銆?MIN_CHART_H = 210

# 涓€涓閲庣暀澶氬皯涓《銆傚睆骞曚笂涓€灞忎篃灏变竴鍗冩潵鍒楋紝杩欐槸"瀛樺浜嗕篃鐢讳笉鍑烘潵"閭ｄ釜涓婇檺銆?BUCKETS_PER_WINDOW = 1000


def bucket_width_ns(seconds, sample_hz):
    """涓€涓閲庨厤澶氬皯绾崇涓€妗躲€?
    涓嬮檺鏄噰鏍烽棿闅旓細妗舵瘮閲囨牱杩樺瘑灏卞彧鏄師鏍峰瓨涓嬫潵锛岀櫧鍗犲唴瀛樸€備笂闄愪笉璁锯€斺€旂獥鍙ｆ媺寰?    瓒婇暱锛屾《瓒婄矖锛岃繖鏄鐨勶紝鐪嬮暱瓒嬪娍鏈潵涔熶笉闇€瑕佹绉掑垎杈ㄧ巼銆?    """
    sample_ns = int(1e9 / max(1.0, sample_hz))
    return max(sample_ns, int(seconds * 1e9 / BUCKETS_PER_WINDOW))


class Series:
    """涓€鏉￠殢鏃堕棿璧扮殑搴忓垪锛屾寜鏃堕棿妗跺瓨鏈€灏?鏈€澶с€傜偣瀛樺湪 array 閲岋紝涓嶆斁 Python 瀵硅薄銆?
    閰嶅鐨勪袱浠朵簨鍦ㄨ繖閲屽仛鎺夛紝鐢荤殑鏃跺€欏氨涓嶅繀鍐嶆兂锛?
      1. **鏂紑**锛氬抚鍙疯烦鍙风殑鍦版柟鏄湡鐨勪涪浜嗘暟鎹紝涓嶆槸"鍊兼病鍙?銆傝烦鍙疯惤鍦ㄥ摢涓《涓?         璁颁竴绗旓紝鐢荤殑鏃跺€欓偅涓《绌虹潃锛屽厤寰楁妸涓ゆ涓嶇浉骞茬殑鏁版嵁杩炴垚涓€鏉＄嚎鈥斺€旈偅鏉＄嚎
         鐪嬬潃鍍?杞村揩閫熻蛋杩囧幓浜?銆?      2. **涓婇檺**锛氬彧鐣欐渶杩?max_buckets 涓《锛屼粠澶撮儴鎴愭壒涓€傚垹 array 鍓?N 涓厓绱犳槸
         O(鍓╀綑闀垮害)锛屾墍浠ユ寜鍧椾涪锛堜竴娆″洓鍒嗕箣涓€锛夎€屼笉鏄瘡鏉ヤ竴涓《涓竴涓€?    """

    def __init__(self, name, color, bucket_ns, max_buckets):
        self.name = name
        self.color = color
        self.bucket_ns = max(1, int(bucket_ns))
        self.max_buckets = max(4, int(max_buckets))
        self.t = array.array("q")       # 妗惰捣鐐癸紙鐩稿浼氳瘽璧风偣锛岀撼绉掞級
        self.lo = array.array("q")
        self.hi = array.array("q")
        self.breaks = array.array("q")  # 璺冲彿鎵€鍦ㄧ殑妗惰捣鐐?        self.last_index = None
        self.disordered = 0             # 鏃堕棿鍊掔潃鏉ョ殑甯ф暟锛堟甯镐负 0锛岃 append锛?
    def __len__(self):
        return len(self.t)

    def __bool__(self):
        return bool(self.t)

    def append(self, stamp_ns, value, publish_index=None):
        bucket = (int(stamp_ns) // self.bucket_ns) * self.bucket_ns
        if publish_index is not None and self.last_index is not None \
                and publish_index != self.last_index + 1:
            self.breaks.append(bucket)
        if publish_index is not None:
            self.last_index = publish_index

        if self.t and bucket < self.t[-1]:
            # 鏃堕棿鍊掔潃鏉ヤ簡銆傛甯歌窇涓嶄細鏈夛紙甯ф寜 publish_index 閫掑鍙栧洖锛宮onotonic_ns
            # 鍗曡皟锛夛紝浣?姝ｅ父涓嶄細鍙戠敓"鏄亣璁句笉鏄繚璇併€傚杩涘幓浼氱牬鍧忔《鐨勬椂闂村簭锛屾憡鍒楁椂
            # 鐢诲嚭楝煎奖锛涙倓鎮勪涪鎺夊垯鏄涪浜嗘暟鎹病浜虹煡閬撱€傛墍浠ヨ涓€绗斻€佷涪寮冦€佸苟涓旇鏁扳€斺€?            # 璁℃暟浼氬嚭鐜板湪鑷鎶ュ憡閲岋紝涓嶄細鐑傚湪鍐呭瓨閲屻€?            self.disordered += 1
            self.breaks.append(bucket)
            return
        if self.t and self.t[-1] == bucket:
            if value < self.lo[-1]:
                self.lo[-1] = value
            if value > self.hi[-1]:
                self.hi[-1] = value
            return
        self.t.append(bucket)
        self.lo.append(value)
        self.hi.append(value)
        if len(self.t) > self.max_buckets:
            drop = max(1, len(self.t) // 4)
            del self.t[:drop]
            del self.lo[:drop]
            del self.hi[:drop]

    def reset(self):
        self.t = array.array("q")
        self.lo = array.array("q")
        self.hi = array.array("q")
        self.breaks = array.array("q")
        self.last_index = None
        self.disordered = 0

    def columns(self, t0, t1, width):
        """鎽婂埌鍍忕礌鍒椾笂锛岃繑鍥?(姣忓垪涓嬬晫, 姣忓垪涓婄晫, 鏂紑鐨勫垪)銆?
        妗舵湰鏉ュ氨姣斿儚绱犲垪灏戯紙涓€涓閲?1000 妗讹級锛屾墍浠ヨ繖閲岃蛋涓€閬嶆《銆佷笉鍋氭帓搴忥紝
        鍚堝苟鏄『甯﹀彂鐢熺殑锛氫袱涓《钀借繘鍚屼竴鍒楀氨鎶婁笂涓嬬晫骞惰捣鏉ャ€?        """
        span = t1 - t0
        if span <= 0 or width <= 0:
            return [], [], set()
        lows = [None] * width
        highs = [None] * width
        scale = width / span
        for stamp, low, high in zip(self.t, self.lo, self.hi):
            if stamp < t0 or stamp > t1:
                continue
            column = int((stamp - t0) * scale)
            if column < 0:
                column = 0
            elif column >= width:
                column = width - 1
            if lows[column] is None:
                lows[column] = low
                highs[column] = high
            else:
                if low < lows[column]:
                    lows[column] = low
                if high > highs[column]:
                    highs[column] = high
        broken = set()
        for stamp in self.breaks:
            if t0 <= stamp <= t1:
                column = int((stamp - t0) * scale)
                if 0 <= column < width:
                    broken.add(column)
        return lows, highs, broken

    def span_ns(self):
        if not self.t:
            return 0
        return self.t[-1] - self.t[0]


class AxisChart(QWidget):
    """涓€鏍硅酱鐨勪笁鏉℃洸绾匡細浣嶇疆/鐩爣銆佽窡闅忚宸€佸姏鐭┿€備笁涓í鏉″叡鐢ㄤ竴鏉℃椂闂磋酱銆?
    鏁版嵁鍙線閲岃拷鍔狅紙append锛夛紝涓嶅湪鐢荤殑鏃跺€欑畻鈥斺€攄raw 閲屽仛鐨勪簨瓒婂皯锛屼富绔欒秺涓嶅彈褰卞搷銆?    """

    def __init__(self, axis_index, seconds=60.0, hz=1000.0, parent=None):
        super().__init__(parent)
        self.axis_index = axis_index
        self.seconds = max(0.5, float(seconds))
        self.bucket_ns = bucket_width_ns(self.seconds, hz)
        # 鐣欎袱涓閲庣殑妗讹細瑙嗛噹鏄竴绐楋紝澶氱暀涓€绐楀厤寰楀垰婊氬嚭鍘荤殑鍙堝洖鏉ヤ簡銆?        max_buckets = 2 * BUCKETS_PER_WINDOW + 8
        self.actual = Series("瀹為檯", COLOR_ACTUAL, self.bucket_ns, max_buckets)
        self.target = Series("鐩爣", COLOR_TARGET, self.bucket_ns, max_buckets)
        self.error = Series("璺熼殢璇樊", COLOR_ERROR, self.bucket_ns, max_buckets)
        self.torque = Series("鍔涚煩", COLOR_TORQUE, self.bucket_ns, max_buckets)
        self.origin_ns = None
        self.latest_ns = None
        self.fed = 0                # 鍠傝繘鏉ョ殑鍘熷甯ф暟锛堝瓨涓嬫潵鐨勬《鏁板皯寰楀锛?        self.gaps = 0

        # 鎽婂垪缁撴灉鐨勭紦瀛橈細鏁版嵁涓€鍙樻垨绐楀彛涓€鍙樺氨澶辨晥銆傛湁浜嗗畠锛岄噸鐢诲彧鏄鍑犱釜鍒楄〃銆?        self._cache = None
        self._cache_key = None
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setMinimumHeight(MIN_CHART_H)

    # ---------------------------------------------------------------- 鏁版嵁
    def append(self, frames):
        grew = False
        for frame in frames:
            stamp = frame["monotonic_ns"]
            if self.origin_ns is None:
                self.origin_ns = stamp
            self.latest_ns = stamp
            relative = stamp - self.origin_ns
            index = frame["publish_index"]
            axis = frame["axes"][self.axis_index]
            actual = axis["actual_position"]
            target = axis["target_position"]
            self.actual.append(relative, actual, index)
            self.target.append(relative, target, index)
            self.error.append(relative, target - actual, index)
            self.torque.append(relative, axis["actual_torque"], index)
            grew = True
        if not grew:
            return
        self.fed += len(frames)
        self.gaps = len(self.actual.breaks)
        self._cache = None
        self.update()

    def clear(self):
        for series in (self.actual, self.target, self.error, self.torque):
            series.reset()
        self.origin_ns = None
        self.latest_ns = None
        self.fed = 0
        self.gaps = 0
        self._cache = None
        self.update()

    def window(self):
        """褰撳墠瑙嗛噹 (t0, t1)锛屽崟浣嶆槸鐩稿绾崇銆?""
        if self.latest_ns is None:
            return 0, 1
        t1 = self.latest_ns - self.origin_ns
        return t1 - int(self.seconds * 1e9), t1

    def stats(self):
        """缁欒嚜妫€鎶ュ憡鐢ㄧ殑瀹炲喌銆?*鏁板瓧閮芥槸浜嬪疄**锛氬杺浜嗗灏戝抚銆佺湡姝ｇ暀浜嗗嚑涓《銆?""
        return {
            "axis": self.axis_index + 1,
            "fed": self.fed,
            "buckets": len(self.actual),
            "bucket_ms": self.bucket_ns / 1e6,
            "span_s": self.actual.span_ns() / 1e9,
            "gaps": self.gaps,
            "disordered": self.actual.disordered,
            "window_s": self.seconds,
        }

    # ---------------------------------------------------------------- 缁樺埗
    def _ensure_columns(self, width):
        key = (self.fed, len(self.actual), width, self.seconds)
        if self._cache is not None and self._cache_key == key:
            return self._cache
        t0, t1 = self.window()
        plot = max(0, width - LEFT_MARGIN - RIGHT_MARGIN)
        panes = {}
        for name, series in (("actual", self.actual), ("target", self.target),
                             ("error", self.error), ("torque", self.torque)):
            panes[name] = series.columns(t0, t1, plot)
        self._cache = (t0, t1, plot, panes)
        self._cache_key = key
        return self._cache

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, False)
        painter.fillRect(self.rect(), COLOR_BG)

        width = self.width()
        height = self.height()
        if width <= LEFT_MARGIN + RIGHT_MARGIN or height <= TOP_MARGIN + BOTTOM_MARGIN:
            painter.setPen(COLOR_TEXT)
            painter.drawText(8, 20, f"杞磠self.axis_index + 1}锛氱獥鍙ｅお灏忥紝鐢讳笉涓?)
            return

        t0, t1, plot, panes = self._ensure_columns(width)
        for pane, top, pane_h in self._pane_bands(height):
            self._draw_pane(painter, pane, top, pane_h, panes)

        self._draw_time_axis(painter, height, t0, t1)

    @staticmethod
    def _pane_bands(height):
        """涓€寮犲浘鐨勯珮搴﹀垏鎴愬摢鍑犱釜妯潯锛歔(妯潯, 涓婅竟, 楂?]锛屾寜浠庝笂鍒颁笅鐨勯『搴忋€?
        鐢诲拰妫€鏌ラ兘璧拌繖涓€浠斤紝妫€鏌ラ噷灏变笉鐢ㄦ妸鍒囨硶鍐嶆妱涓€閬嶁€斺€旀妱鐨勯偅浠戒竴鏃﹁窡杩欓噷璧板矓浜嗭紝
        妫€鏌ヤ細缁跨潃鑰屽睆骞曚笂涓嶆槸閭ｄ箞鍥炰簨銆?        """
        plot_height = height - TOP_MARGIN - BOTTOM_MARGIN
        weight_sum = sum(PANE_HEIGHT_WEIGHT)
        bands = []
        top = TOP_MARGIN
        for pane in PANE_ORDER:
            pane_h = int(plot_height * PANE_HEIGHT_WEIGHT[pane] / weight_sum)
            bands.append((pane, top, pane_h))
            top += pane_h
        return bands

    def _pane_series(self, pane, panes):
        if pane == PANE_ACTUAL:
            return (("actual", panes["actual"], COLOR_ACTUAL),
                    ("target", panes["target"], COLOR_TARGET))
        if pane == PANE_ERROR:
            return (("error", panes["error"], COLOR_ERROR),)
        return (("torque", panes["torque"], COLOR_TORQUE),)

    def _draw_frame(self, painter, pane, top, pane_h, entries):
        """鐢讳竴涓í鏉＄殑澶栨涓庢爣棰橈紝骞舵妸绾佃酱鑼冨洿绠楀嚭鏉ャ€傝寖鍥寸畻涓嶅嚭鏉ュ氨杩斿洖 None銆?""
        painter.setPen(QPen(COLOR_AXIS, 1, Qt.DotLine))
        painter.drawLine(LEFT_MARGIN, top + pane_h, self.width() - RIGHT_MARGIN,
                         top + pane_h)
        painter.setPen(COLOR_TEXT)
        painter.drawText(6, top + 12, f"杞磠self.axis_index + 1} {PANE_TITLES[pane]}")

        lo = hi = None
        for _name, (lows, highs, _broken), _color in entries:
            for value in lows:
                if value is not None and (lo is None or value < lo):
                    lo = value
            for value in highs:
                if value is not None and (hi is None or value > hi):
                    hi = value
        if lo is None:
            painter.setPen(COLOR_AXIS)
            painter.drawText(LEFT_MARGIN + 8, top + pane_h // 2, "杩樻病鏈夋暟鎹?)
            return None
        if hi <= lo:
            # 涓€鏉″钩鐩寸殑绾匡細缁欎釜涓婁笅鍚勪竴鐐圭殑浣欓噺锛屽惁鍒欓櫎浠ラ浂銆?            lo, hi = lo - 1, hi + 1
        pad = (hi - lo) * 0.08
        lo -= pad
        hi += pad
        return lo, hi

    @staticmethod
    def _pane_geometry(top, pane_h):
        """涓€涓í鏉＄殑鍑犱綍锛氭洸绾垮尯鐨勪笂涓嬭竟銆佹爣棰樼殑鍩虹嚎銆?
        鏇茬嚎鍖轰笂涓嬪悇璁╁嚭 `HEADROOM`锛氭瀬鍊艰创鐫€杈规绾夸笉濂界湅锛岃€屼笖涓婄鐨勫埢搴﹀€兼槸鐢诲湪
        鏇茬嚎鍖轰笂杈?*寰€涓?* `LABEL_DROP` 澶勭殑锛屼笉鐣欒繖鐐瑰畠灏卞嚭鐣屼簡銆備笂杈瑰彟澶栬繕璁╁嚭
        鏍囬閭ｄ竴鏉?`TITLE_H`銆?
        **涓婄鍒诲害鍊肩殑鍩虹嚎蹇呴』钀藉湪鏍囬鍩虹嚎浠ヤ笅銆?* 鏍囬鍜屽埢搴﹀€奸兘浠?x=6 璧风敾锛屾爣棰?        鍦?`top + 12`锛涙洸绾垮尯涓婅竟瑕佹槸璺戝埌鏍囬鍩虹嚎涓婇潰鍘伙紙`TITLE_H` 娌¤鍑烘潵灏辨槸锛夛紝
        鍒诲害鍊煎氨鍘嬪湪鏍囬涓娾€斺€擯12.18 灏辨槸杩欎釜锛屼袱涓彧宸竴涓儚绱狅紝鐪嬬潃鍍忓瓧绯婁簡銆?        鍑犱綍鍗曠嫭鎷垮嚭鏉ワ紝灏辨槸涓轰簡璁?`checks/chart_check.py` 鑳介拤浣忚繖涓€鏉°€?        """
        title_baseline = top + 12
        plot_top = top + TITLE_H + HEADROOM
        plot_bottom = top + pane_h - HEADROOM
        return plot_top, plot_bottom, title_baseline

    def _draw_pane(self, painter, pane, top, pane_h, panes):
        # 杩炴爣棰樺甫涓€娈垫洸绾块兘鏀句笉涓嬪氨鏁翠釜妯潯涓嶇敾鈥斺€旂敾浜嗕篃鍙槸涓€鏉″帇鍦ㄤ竴璧风殑瀛椼€?        if pane_h < max(MIN_PANE_H, TITLE_H + MIN_PLOT_H):
            return
        entries = self._pane_series(pane, panes)
        bounds = self._draw_frame(painter, pane, top, pane_h, entries)
        if bounds is None:
            return
        lo, hi = bounds
        span = hi - lo
        plot_top, plot_bottom, _title = self._pane_geometry(top, pane_h)
        inner = max(1, plot_bottom - plot_top)

        def to_y(value):
            return plot_bottom - int((value - lo) * inner / span)

        painter.setPen(COLOR_TEXT)
        painter.drawText(6, to_y(hi) + LABEL_DROP, f"{int(hi)}")
        painter.drawText(6, to_y(lo) + LABEL_DROP, f"{int(lo)}")

        for _name, (lows, highs, broken), color in entries:
            painter.setPen(QPen(color, 1))
            # 涓€鍒椾竴鏉＄珫绾匡紝浠庤繖涓€鍒楃殑鏈€灏忓€肩敾鍒版渶澶у€笺€傛瀬鍊奸兘鍦紝鍖呯粶灏变涪涓嶄簡銆?            for column, low in enumerate(lows):
                if low is None or column in broken:
                    continue        # 娌℃湁鏁版嵁鐨勫垪銆佽烦鍙锋墍鍦ㄧ殑鍒楋紝閮界┖鐫€
                high = highs[column]
                x = LEFT_MARGIN + column
                if low == high:
                    painter.drawPoint(QPoint(x, to_y(low)))
                else:
                    painter.drawLine(x, to_y(low), x, to_y(high))

    def _draw_time_axis(self, painter, height, t0, t1):
        plot_width = self.width() - LEFT_MARGIN - RIGHT_MARGIN
        if plot_width <= 0:
            return
        painter.setPen(COLOR_AXIS)
        y = height - BOTTOM_MARGIN + 2
        painter.drawLine(LEFT_MARGIN, y, LEFT_MARGIN + plot_width, y)
        painter.setPen(COLOR_TEXT)
        metrics = QFontMetrics(painter.font())
        # 鍙爣璧锋鍜屼腑闂达細鍒诲害鍐嶅瘑鍦ㄨ繖鍧楀湴鏂逛篃璇讳笉娓呫€?        span_s = (t1 - t0) / 1e9
        for fraction, text, align in ((0.0, f"-{span_s:.1f}s", "left"),
                                      (0.5, f"-{span_s / 2:.1f}s", "center"),
                                      (1.0, "鐜板湪", "right")):
            x = LEFT_MARGIN + int(plot_width * fraction)
            offset = {"left": 0,
                      "center": metrics.width(text) // 2,
                      "right": metrics.width(text)}[align]
            painter.drawText(x - offset, y + 14, text)

    def mousePressEvent(self, event):
        """鐐逛竴涓嬫竻绌衡€斺€旂湅涓€娈垫柊鍔ㄤ綔涔嬪墠涓嶇敤閲嶅惎鐣岄潰銆?""
        if event.button() == Qt.LeftButton:
            self.clear()


class ChartPanel(QWidget):
    """浜旀牴杞寸殑鏇茬嚎锛屼竴娆℃樉绀洪€変腑鐨勯偅涓€鏍广€傚垏鎹㈢敱澶栭潰鐨勪笅鎷夎礋璐ｏ紙瑙?app.py锛夈€?
    **涓轰粈涔堜笉鍋氭垚浜斿紶骞舵帓**锛氫笁鏉℃洸绾?脳 浜旀牴杞?= 鍗佷簲鏉＄嚎鎸ゅ湪涓€灞忛噷娌℃硶璇伙紝鑰屽彴鏋朵笂
    瑕佸洖绛旂殑闂鏄?杩欎竴鏍硅酱杩欐璧板緱瀵逛笉瀵?銆傛墍浠ヨ鐪嬪緱杩囨潵锛岃€屼笉鏄湅寰楀叏銆?
    **涓€娆″彧鍠傚綋鍓嶈繖涓€鏍硅酱銆?* 浜旀牴鍏ㄦ敹鏄簲鍊嶇殑鍐呭瓨鍜屼簲鍊嶇殑寮€閿€锛岃€屽睆骞曚笂涓€娆″彧鐪嬪緱
    鍒颁竴鏍广€備唬浠锋槸鍒囪酱鏃舵洸绾夸粠鍒囩殑閭ｄ竴鍒诲紑濮嬧€斺€旂晫闈笂鏍囨敞浜嗚繖涓€鐐广€?    """

    def __init__(self, axis_count, seconds=60.0, hz=1000.0, parent=None):
        super().__init__(parent)
        # 浜斿紶鍥鹃兘濉炶繘鍚屼竴涓牸瀛愶紝闈?show/hide 鍒囨崲銆傛病鏄剧ず鐨勯偅鍑犲紶涓嶇敾銆佷篃涓嶆敹鏁版嵁銆?        stack = QVBoxLayout(self)
        stack.setContentsMargins(0, 0, 0, 0)
        self.charts = []
        self.current = 0
        for index in range(axis_count):
            chart = AxisChart(index, seconds=seconds, hz=hz)
            chart.hide()
            stack.addWidget(chart)
            self.charts.append(chart)
        if self.charts:
            self.charts[0].show()
        # 涓€娆″彧鏄剧ず涓€寮狅紝鎵€浠ヨ繖涓鍣ㄧ殑鏈€灏忛珮搴﹀氨鏄?*涓€寮?*鍥剧殑鏈€灏忛珮搴︺€?        # 涓嶅啓杩欎竴鍙ョ殑璇濓紝澶栧眰甯冨眬缁欒繖涓鍣ㄧ殑浠介浼氬皯浜庨噷闈㈤偅寮犲浘瑕佺殑锛堝疄娴?121 瀵?        # 170锛夛紝涓変釜妯潯灏辫鍘嬫垚涓夋潯缁嗙紳鈥斺€旇€屾洸绾挎伆鎭版槸杩欎竴灞傛渶璇ョ湅娓呮鐨勪笢瑗裤€?        self.setMinimumHeight(MIN_CHART_H)

    def append(self, frames):
        if 0 <= self.current < len(self.charts):
            self.charts[self.current].append(frames)

    def select(self, index):
        if not (0 <= index < len(self.charts)) or index == self.current:
            return
        self.charts[self.current].hide()
        self.current = index
        self.charts[index].show()
        self.charts[index].update()

    def active(self):
        """褰撳墠鏄剧ず鐨勯偅鏍硅酱鐨勫浘琛紱涓€鏍硅酱閮芥病鏈夋椂鏄?None銆?""
        return self.charts[self.current] if self.charts else None

    def clear(self):
        for chart in self.charts:
            chart.clear()
