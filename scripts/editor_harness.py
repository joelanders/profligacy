#!/usr/bin/env python3
"""Headless visual QA for the WebView editor.

Renders src/editor/index.html in headless Chrome with the JUCE native bridge
mocked from REAL machine data (patch names + program records read straight from
the sysram NVRAM file, plus canned live-LCD lines), and screenshots one or more
driven UI states. No DAW, no window server, no MAME boot needed.

  python3 scripts/editor_harness.py                 # all states -> ./editor_shots/
  python3 scripts/editor_harness.py --states b52    # one state
  python3 scripts/editor_harness.py --states program,eg,lfoview --font-sizes 100,145
  python3 scripts/editor_harness.py --states program,big --sizes 740x360,1184x576,1560x900
  SYSRAM=/path/to/sysram python3 scripts/editor_harness.py

Each capture also audits visible deep-editor geometry and text scaling, and exits
nonzero if a control escapes its row, grid items overlap, a non-scrollable row
overflows, or visible Edit-view text ignores the requested scale.

States: default (hardware panel, A00) / browser (patch list open) / b52
(scripted drive: open browser, click B52, resync) / norom / nonames / matrix /
route / fixed / material / program / oscillator / wave / amplifier /
arp / arpedit / global
(see STATES below for each).
"""
import argparse, base64, html as html_lib, json, os, pathlib, re, subprocess, sys

REPO = pathlib.Path(__file__).resolve().parents[1]
CHROME = os.environ.get("CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
SYSRAM = pathlib.Path(os.environ.get("SYSRAM", REPO / "../mame/nvram/korgprop/sysram"))
BASE, REC = 0x20A10, 535  # firmware v1.7 battery-RAM bank layout (128 records, name = first 16 bytes)

LCD = {
    "0":   ["A00:Prophetic Steps! [ Motion ] PAT:UP  ", "Sd1LFOI --OFF-- MNsLvl1 PrtTime DlyBal "],
    "116": ["B52:303Growler Rbn~  [SynthBss] PAT:UP  ", "--OFF--  Amp1   --OFF-- --OFF-- --OFF-- "],
}

SHIM = """<script>
// headless test shim: emulates the JUCE WebView backend with real machine data
const MOCK = __HARNESS_MOCK__;
(function(){
  // Set state flags SYNCHRONOUSLY (before the editor scripts run checkRom), so no-ROM /
  // no-names states are in effect for the editor's very first getRomStatus/getPatchNames.
  const q0 = new URLSearchParams(location.search).get("do");
  try { localStorage.removeItem("prophecy.editor.appearance.v2"); } catch (_) {}
  if (q0 === "norom") MOCK.norom = true;
  if (q0 === "nonames") MOCK.nonames = true;
  if (q0 === "lfotiming") {
    // Four deliberately different timing cases: fade-in at key-on, fade-out at
    // key-off, BOTH mode, and free-running Key Sync OFF (delay/fade inactive).
    const rec=MOCK.records["0"];
    [[94,0,0,1,50,0,35,70], [105,4,1,1,100,20,30,-60],
     [116,8,2,1,150,-20,15,45], [127,12,0,0,199,0,80,80]].forEach(c=>{
      const [base,wave,mode,sync,freq,offset,delay,fade]=c;
      rec[base]=(wave&31)|((mode&3)<<5)|(sync?128:0); rec[base+1]=freq;
      rec[base+6]=offset&255; rec[base+9]=delay; rec[base+10]=fade&255;
    });
  }
  let patch = 0, dumpVer = 1, arpPattern = 5, arpVer = 1;
  const listeners = {};
  window.__JUCE__ = { backend: {
    addEventListener(name, fn){ (listeners[name] = listeners[name] || []).push(fn); },
    emitEvent(name, payload){
      if (name !== "__juce__invoke") return;
      const {name: fn, params, resultId} = payload;
      let result = null;
      if (fn === "getLcd") { const l = MOCK.lcd[String(patch)] || MOCK.lcd["0"]; result = {line1:l[0], line2:l[1]}; }
      else if (fn === "getPatchNames") result = MOCK.nonames ? [] : MOCK.names;
      else if (fn === "getProgramData") { const r = MOCK.records[String(patch)] || MOCK.records["0"]; result = {version: dumpVer, bytes: r}; }
      else if (fn === "getArpPatternData") result = {version:arpVer, pattern:arpPattern, bytes:MOCK.arp};
      else if (fn === "getRomStatus") result = {ok: MOCK.norom ? false : true, path:"(headless harness)"};
      else if (fn === "requestDump") { dumpVer++; }
      else if (fn === "requestArpPatternDump") { arpVer++; }
      else if (fn === "selectArpPattern") { arpPattern = params[0]|0; }
      else if (fn === "selectPatch") { patch = params[0]|0; }
      else if (fn === "panelPulse") { console.log("panelPulse", JSON.stringify(params)); }
      else if (fn === "getLcdRaw") result = {version: 0};       // text-fallback LCD in harness
      else if (fn === "getLeds") result = {version: 0, banks: []};
      else if (fn === "setAdin") {}
      else if (fn === "sendMidi") { console.log("sendMidi", JSON.stringify(params)); }
      else if (fn === "setParam") {
        // Mirror the structural OSC Set write so the editor's delayed read-back sees
        // the state it just requested instead of restoring the original fixture byte.
        if ((params[0]|0) === 154) (MOCK.records[String(patch)] || MOCK.records["0"])[138] = params[1]|0;
        console.log("setParam", JSON.stringify(params));
      }
      else if (fn === "setPatternParam" || fn === "setArpControl" || fn === "sendArpPatternData") { console.log(fn, JSON.stringify(params)); }
      else if (fn === "setGlobalParam") { console.log("setGlobalParam", JSON.stringify(params)); }
      setTimeout(() => (listeners["__juce__complete"]||[]).forEach(f => f({promiseId: resultId, result})), 5);
    },
  }};
  window.addEventListener("load", () => setTimeout(() => {
    const q = new URLSearchParams(location.search).get("do");
    if (["program","oscillator","brass","reed","wave","amplifier","ampvelocity","eg","lfoview","lfotiming","name","mixer","big","bigwave","appearance"].includes(q))
      document.getElementById("chip_program")?.click();
    if (q === "arp" || q === "arpedit") document.getElementById("chip_arp")?.click();
    if (q === "global") document.getElementById("chip_global")?.click();
    if (q === "arpedit") {
      setTimeout(()=>document.getElementById("arpunlock")?.click(),900);
    }
    if (["oscillator","brass","reed"].includes(q)) document.querySelector('.flowblock[data-target="oscillators"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "brass" || q === "reed") setTimeout(() => {
      document.querySelectorAll(".oscset")[q === "brass" ? 9 : 10]?.click();
    }, 900);
    if (q === "brass" || q === "reed") setTimeout(() => {
      document.querySelector(".oscsets")?.scrollIntoView({block:"start"});
    }, 1300);
    if (q === "eg") document.querySelector('.flowblock[data-target="eg"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "mixer") document.querySelector('.flowblock[data-target="mixer"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "lfoview" || q === "lfotiming") document.querySelector('.flowblock[data-target="lfo"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "name") setTimeout(() => {
      const s=document.getElementById("deepsearch"); s.value="program name";
      s.dispatchEvent(new Event("input",{bubbles:true}));
    }, 300);
    if (q === "eg" || q === "lfoview" || q === "lfotiming" || q === "name" || q === "mixer" || q === "big" || q === "bigwave") setTimeout(() => {
      document.querySelector("#deeproot .deepsub")?.scrollIntoView({block:"start"});
    }, 800);
    if (q === "wave") document.querySelector('.flowblock[data-target="waveshape"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "amplifier" || q === "ampvelocity") document.querySelector('.flowblock[data-target="amplifier"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "ampvelocity") setTimeout(() => {
      const heading=[...document.querySelectorAll(".deepcluster h4")]
        .find(el=>el.textContent.toLowerCase().includes("velocity control"));
      heading?.scrollIntoView({block:"center"});
    }, 400);
    if (q === "appearance") document.getElementById("editlooktoggle")?.click();
    const requestedFont=Number(new URLSearchParams(location.search).get("font"));
    if (Number.isFinite(requestedFont)) {
      const f=document.getElementById("editfont");
      if(f){f.value=Math.max(50,Math.min(200,requestedFont));f.dispatchEvent(new Event("input",{bubbles:true}));}
    }
    if (q === "big" || q === "bigwave") {
      const f=document.getElementById("editfont"); if(f){f.value=145;f.dispatchEvent(new Event("input",{bubbles:true}));}
    }
    if (q === "bigwave") document.querySelector('.flowblock[data-target="waveshape"]')
      ?.dispatchEvent(new MouseEvent("click",{bubbles:true}));
    if (q === "matrix" || q === "route" || q === "fixed") document.getElementById("chip_matrix")?.click();
    if (q === "material") document.getElementById("chip_material")?.click();
    if (q === "dev") document.getElementById("chip_dev")?.click();
    if (q === "fixed") setTimeout(() => {
      document.getElementById("mmsync")?.click();
      setTimeout(() => document.getElementById("fxtoggle")?.click(), 700);
    }, 300);
    if (q === "route") setTimeout(() => {
      document.getElementById("mmsync")?.click();     // force a sync now
      setTimeout(() => {
        // Pick a genuinely free compatible slot. The repaired UI intentionally does
        // not mark cells that would replace an already-active route as routable.
        const td = document.querySelector('td.mmroute:not(:has(.mmcell))');
        td?.dispatchEvent(new PointerEvent("pointerdown", {bubbles:true}));
      }, 700);
    }, 300);
    if (q === "browser" || q === "b52") document.getElementById("pno").click();
    if (q === "b52") setTimeout(() => { document.querySelector('#blist [data-i="116"]')?.click(); }, 400);
    if (q === "nonames") document.getElementById("pno")?.click();
  }, 900));

  // Machine-readable geometry audit. Screenshots are useful review artefacts, but a
  // slider crossing its row boundary must make the harness fail automatically.
  window.addEventListener("load", () => setTimeout(() => {
    if (new URLSearchParams(location.search).get("audit") !== "1") return;
    const visible=el=>{
      const r=el.getBoundingClientRect(), s=getComputedStyle(el);
      return s.display!=="none" && s.visibility!=="hidden" && r.width>0 && r.height>0;
    };
    const inViewport=el=>{
      if(!visible(el)) return false;
      const r=el.getBoundingClientRect();
      return r.right>0 && r.bottom>0 && r.left<innerWidth && r.top<innerHeight;
    };
    const issues=[];
    let rows=0, controls=0;
    const auditState=new URLSearchParams(location.search).get("do")||"";
    const editBody=document.querySelector("#editview>.body"), backTop=document.getElementById("editbacktop");
    if(editBody.scrollTop>=240 && backTop.hidden) issues.push("back-to-top button hidden after editor scroll");
    if(auditState==="program") {
      const flow=document.querySelector(".flowdiagram");
      if(!flow || flow.querySelectorAll(".flowblock").length!==10 ||
         flow.querySelectorAll(".moddestination").length!==6 ||
         flow.querySelectorAll(".modrail").length!==1 || flow.querySelector(".control"))
        issues.push("signal flow is missing the consolidated modulation-bus structure");
      const flowHost=document.getElementById("programflow");
      if(flowHost && flowHost.scrollWidth>flowHost.clientWidth+1)
        issues.push(`signal flow horizontal overflow ${flowHost.scrollWidth}>${flowHost.clientWidth}`);
      const source=flow&&flow.querySelector('[data-target="eg"]');
      source?.dispatchEvent(new MouseEvent("mouseenter"));
      if(flow&&!flow.classList.contains("mod-active")) issues.push("modulation bus does not highlight from its source");
      source?.dispatchEvent(new MouseEvent("mouseleave"));
      if(flow?.querySelectorAll(".feedback.self").length!==2)
        issues.push("waveshaper self-feedback loops are incomplete");
      if(flow?.querySelectorAll(".feedback.cross").length!==2)
        issues.push("waveshaper cross-feedback paths are incomplete");
      if(flow?.querySelectorAll(".feedback.return.postamp").length!==1)
        issues.push("mixer feedback return is not explicitly post-amplifier");
    }
    if(auditState==="oscillator") {
      const sets=document.querySelectorAll(".oscset");
      const panels=[...document.querySelectorAll("#deeproot>.deepsection")];
      const activeSet=(MOCK.records[String(patch)] || MOCK.records["0"])[138] | 0;
      const expected=OSC_SETS[activeSet].filter(Boolean).length;
      if(sets.length!==12) issues.push(`OSC Set button count ${sets.length}!=12`);
      if(document.querySelector(".oscslot,.engine,.engines"))
        issues.push("oscillator implementation selectors are still visible");
      if(panels.length!==expected || panels.some(panel=>!panel.open))
        issues.push(`active oscillator panels ${panels.length}/${expected} are not all expanded`);
    }
    if(auditState==="arpedit") {
      const cells=document.querySelectorAll("#arpseq .arpcell");
      const bars=document.querySelectorAll("#arpseq .arpbar[role=slider]");
      const bipolar=document.querySelectorAll("#arpseq .arpbar.bipolar .arpbarzero");
      if(cells.length!==96) issues.push(`ARP step cell count ${cells.length}!=96`);
      if(bars.length!==72) issues.push(`ARP drawable bar count ${bars.length}!=72`);
      if(bipolar.length!==24) issues.push(`ARP bipolar bar count ${bipolar.length}!=24`);
    }
    document.querySelectorAll("#editview select").forEach(select=>{
      if(visible(select) && select.options.length>=2 && select.options.length<=6 && select.id!=="mmiaddsource")
        issues.push(`small ${select.options.length}-choice select was not upgraded: ${select.id||select.className||"unnamed"}`);
    });
    if(auditState==="lfoview") {
      const pickers=[...document.querySelectorAll(".lfopicker")];
      if(pickers.length!==4) issues.push(`LFO picker count ${pickers.length}!=4`);
      pickers.forEach((picker,index)=>{
        const families=picker.querySelectorAll(".lfofamilyrow .choicebtn");
        const selected=picker.querySelectorAll(".lfofamilyrow .choicebtn.on");
        if(families.length!==17) issues.push(`LFO picker ${index+1} family count ${families.length}!=17`);
        if(selected.length!==1) issues.push(`LFO picker ${index+1} selected families ${selected.length}!=1`);
        const variants=picker.querySelector(".lfovariants:not([hidden])");
        if(variants && variants.querySelectorAll(".choicebtn.on").length!==1)
          issues.push(`LFO picker ${index+1} has no selected phase/variant`);
      });
    }
    document.querySelectorAll("#editview .deeprow").forEach((row,index)=>{
      if(!visible(row)) return;
      rows++;
      const rr=row.getBoundingClientRect();
      if(row.scrollWidth>row.clientWidth+1)
        issues.push(`row ${index} horizontal overflow ${row.scrollWidth}>${row.clientWidth}`);
      const items=[...row.children].filter(visible);
      for(let a=0;a<items.length;a++) for(let b=a+1;b<items.length;b++){
        const x=items[a].getBoundingClientRect(), y=items[b].getBoundingClientRect();
        if(Math.min(x.right,y.right)-Math.max(x.left,y.left)>1 &&
           Math.min(x.bottom,y.bottom)-Math.max(x.top,y.top)>1)
          issues.push(`row ${index} grid items overlap`);
      }
      row.querySelectorAll("input,select,button").forEach(control=>{
        if(!visible(control)) return;
        controls++;
        const cr=control.getBoundingClientRect();
        if(cr.left<rr.left-1 || cr.right>rr.right+1)
          issues.push(`row ${index} ${control.tagName.toLowerCase()} escapes horizontally`);
      });
    });
    document.querySelectorAll("#editview .deepsub,#editview .deepctl").forEach((el,index)=>{
      if(visible(el) && el.scrollWidth>el.clientWidth+1)
        issues.push(`${el.className} ${index} horizontal overflow ${el.scrollWidth}>${el.clientWidth}`);
    });
    if(auditState==="lfotiming") {
      const expected=[
        ["LFO1","ON","ON","35","70","35","70","key on","fade in"],
        ["LFO2","OFF","ON","30","-60","30","-60","key off","fade out"],
        ["LFO3","BOTH","ON","15","45","15","45","key on","fade in"],
        ["LFO4","ON","OFF","80","80","0","0","key on","no fade"]
      ];
      const previews=[...document.querySelectorAll('svg[data-lfo-preview="1"]')];
      if(previews.length!==4) issues.push(`LFO timing preview count ${previews.length}!=4`);
      expected.forEach((want,index)=>{
        const d=previews[index]?.dataset;
        if(!d) return;
        const got=[d.lfoSub,d.lfoMode,d.lfoKeySync,d.lfoDelay,d.lfoFade,
          d.lfoEffectiveDelay,d.lfoEffectiveFade,d.lfoTrigger,d.lfoFadeDirection];
        if(got.some((value,i)=>value!==want[i]))
          issues.push(`LFO timing ${index+1} ${JSON.stringify(got)}!=${JSON.stringify(want)}`);
        if(index<3 && !previews[index].querySelector(".lfodelay"))
          issues.push(`LFO timing ${index+1} missing delay region`);
        if(index===3 && previews[index].querySelector(".lfodelay"))
          issues.push("LFO timing Key Sync OFF incorrectly shows effective delay");
      });
    }
    // Sample visible text at 100% and at the requested scale. This catches future
    // components whose fixed px/font shorthand is not wired to the editor variable.
    const editView=document.getElementById("editview");
    const scale=parseFloat(getComputedStyle(editView).getPropertyValue("--editor-font-scale"))||1;
    const textElements=[...editView.querySelectorAll("*")].filter(el=>{
      if(!inViewport(el)) return false;
      // Responsive SVG diagrams stay page-width at every text setting; scaling the
      // entire SVG would reintroduce horizontal scrolling. Audit their fit separately.
      if(el instanceof SVGElement) return false;
      if(el.matches("input:not([type=range]):not([type=checkbox]),select,button,output")) return true;
      return [...el.childNodes].some(n=>n.nodeType===Node.TEXT_NODE && n.textContent.trim());
    });
    const renderedFontSize=el=>{
      const cssSize=parseFloat(getComputedStyle(el).fontSize);
      if(!(el instanceof SVGTextElement)) return cssSize;
      const svg=el.ownerSVGElement, viewWidth=svg?.viewBox?.baseVal?.width;
      return viewWidth ? cssSize*svg.getBoundingClientRect().width/viewWidth : cssSize;
    };
    const setAuditScale=value=>{
      editView.style.setProperty("--editor-font-scale",value);
    };
    const scaledSizes=textElements.map(renderedFontSize);
    setAuditScale(1);
    const baseSizes=textElements.map(renderedFontSize);
    setAuditScale(scale);
    let text=0;
    textElements.forEach((el,index)=>{
      text++;
      const expected=baseSizes[index]*scale;
      if(Math.abs(scaledSizes[index]-expected)>.2) {
        const tag=el.tagName.toLowerCase();
        const ident=el.id ? `#${el.id}` : el.classList.length ? `.${[...el.classList].join(".")}` : "";
        issues.push(`text ${tag}${ident} stays ${scaledSizes[index].toFixed(2)}px; expected ${expected.toFixed(2)}px`);
      }
    });
    const result={width:innerWidth,height:innerHeight,
      font:document.getElementById("editfont")?.value||null,rows,controls,text,issues};
    const out=document.createElement("script");
    out.id="layout-audit-result"; out.type="application/json";
    out.textContent=JSON.stringify(result); document.body.appendChild(out);
  }, 3000));
})();
</script>
"""

STATES = {"default": "", "norom": "?do=norom", "nonames": "?do=nonames", "matrix": "?do=matrix",
          "route": "?do=route", "fixed": "?do=fixed", "material": "?do=material", "dev": "?do=dev",
          "program": "?do=program", "oscillator": "?do=oscillator",
          "brass": "?do=brass", "reed": "?do=reed",
          "wave": "?do=wave", "amplifier": "?do=amplifier", "ampvelocity": "?do=ampvelocity",
          "big": "?do=big", "bigwave": "?do=bigwave", "appearance": "?do=appearance",
          "arp": "?do=arp", "arpedit": "?do=arpedit", "global": "?do=global",
          "eg": "?do=eg", "lfoview": "?do=lfoview", "lfotiming": "?do=lfotiming",
          "name": "?do=name", "mixer": "?do=mixer",
          "browser": "?do=browser", "b52": "?do=b52"}


def build_mock() -> dict:
    data = SYSRAM.read_bytes()
    names, records = [], {}
    for i in range(128):
        rec = data[BASE + i * REC : BASE + (i + 1) * REC]
        names.append("".join(chr(b) if 32 <= b < 127 else " " for b in rec[:16]).rstrip())
    for i in (0, 116):
        records[str(i)] = list(data[BASE + i * REC : BASE + (i + 1) * REC])
    arp = [0] * 128
    arp[17] = 2      # eighth-note step base
    arp[18] = 1      # sort on
    arp[19] = 0      # C-1
    arp[20] = 127    # G9
    arp[21] = 129    # per-step velocity
    arp[24] = 101    # per-step gate
    arp[27] = 2      # Running Up
    arp[28] = 0      # octave alternation Up
    for i in range(24):
        base = 32 + i * 4
        arp[base + 0] = 0
        arp[base + 1] = (i % 4) + 1
        arp[base + 2] = 120 if i % 2 else 48
        arp[base + 3] = 80 if i % 3 else 0
    arp[32 + 8 * 4 + 1] = 13  # LOOP on step 9
    return {"names": names, "records": records, "lcd": LCD, "arp": arp}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--states", default=",".join(STATES), help="comma list: " + " ".join(STATES))
    ap.add_argument("--out", type=pathlib.Path, default=REPO / "editor_shots")
    ap.add_argument("--sizes", default="900x560",
                    help="comma list of viewport sizes, e.g. 740x560,1184x576,1560x900")
    ap.add_argument("--font-sizes", default="100",
                    help="comma list of Edit-view text percentages, e.g. 100,130,145")
    ap.add_argument("--skip-layout-audit", action="store_true",
                    help="capture screenshots without failing on deep-editor geometry errors")
    args = ap.parse_args()

    sizes = []
    try:
        for item in args.sizes.split(","):
            width, height = (int(value) for value in item.lower().split("x", 1))
            if width < 1 or height < 1:
                raise ValueError
            sizes.append((width, height))
    except ValueError:
        print(f"error: invalid --sizes value: {args.sizes}", file=sys.stderr)
        return 2

    font_sizes = []
    try:
        for item in args.font_sizes.split(","):
            font_size = int(item)
            if not 50 <= font_size <= 200:
                raise ValueError
            font_sizes.append(font_size)
    except ValueError:
        print(f"error: invalid --font-sizes value: {args.font_sizes} (expected 50..200)",
              file=sys.stderr)
        return 2

    if not SYSRAM.exists():
        print(f"error: sysram not found at {SYSRAM} (set SYSRAM=)", file=sys.stderr)
        return 1
    if not pathlib.Path(CHROME).exists():
        print(f"error: Chrome not found at {CHROME} (set CHROME=)", file=sys.stderr)
        return 1

    html = (REPO / "src/editor/index.html").read_text()
    # The plugin serves this generated catalogue as a BinaryData resource.  file://
    # cannot resolve that route, so inline it for the standalone browser harness.
    manifest = (REPO / "src/editor/deep_editor_manifest.js").read_text()
    html = html.replace('<script src="/assets/deep_editor_manifest.js"></script>',
                        "<script>" + manifest + "</script>")
    # file:// can't resolve the plugin's resource-provider font route — inline it.
    font = REPO / "src/editor/assets/NotoSans-Bold.ttf"
    if font.exists():
        html = html.replace("url('/assets/NotoSans-Bold.ttf')",
                            "url('data:font/ttf;base64," + base64.b64encode(font.read_bytes()).decode() + "')")
    shim = SHIM.replace("__HARNESS_MOCK__", json.dumps(build_mock()))
    harness = html.replace("<script>", shim + "<script>", 1)
    args.out.mkdir(parents=True, exist_ok=True)
    hpath = args.out / "harness.html"
    hpath.write_text(harness)
    # Never attach headless runs to the user's live Chrome profile. Modern Chrome
    # aborts a second process before loading the page when the default profile is
    # already owned by the interactive browser.
    chrome_profile = args.out / "chrome-profile"
    chrome_profile.mkdir(parents=True, exist_ok=True)

    failed = False
    for raw_name in args.states.split(","):
        name = raw_name.strip()
        if name not in STATES:
            print(f"error: unknown state: {name}", file=sys.stderr)
            return 2
        for width, height in sizes:
            for font_size in font_sizes:
                size_suffix = f"_{width}x{height}" if len(sizes) > 1 else ""
                font_suffix = f"_font{font_size}" if len(font_sizes) > 1 else ""
                shot = args.out / f"editor_{name}{size_suffix}{font_suffix}.png"
                q = STATES[name]
                q += ("&" if q else "?") + f"font={font_size}"
                if not args.skip_layout_audit:
                    q += "&audit=1"
                command = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
                           "--no-default-browser-check", f"--user-data-dir={chrome_profile}",
                           "--force-device-scale-factor=2",
                           f"--window-size={width},{height}", "--virtual-time-budget=4000",
                           f"--screenshot={shot}"]
                if not args.skip_layout_audit:
                    command.append("--dump-dom")
                command.append(f"file://{hpath}{q}")
                try:
                    completed = subprocess.run(command, check=True, capture_output=True, text=True,
                                               timeout=12)
                except subprocess.TimeoutExpired as exc:
                    # Chrome 151 can finish --screenshot/--dump-dom and then keep the
                    # headless browser alive because this UI intentionally owns polling
                    # timers. subprocess.run has already killed and reaped it here; if
                    # the requested artifact and DOM output exist, validate those rather
                    # than misreporting a completed capture as a layout failure.
                    stdout = exc.stdout or ""
                    stderr = exc.stderr or ""
                    if isinstance(stdout, bytes): stdout = stdout.decode(errors="replace")
                    if isinstance(stderr, bytes): stderr = stderr.decode(errors="replace")
                    if shot.exists():
                        completed = subprocess.CompletedProcess(command, 0, stdout, stderr)
                    else:
                        print(f"FAIL {name} {width}x{height} font={font_size}: Chrome {exc}",
                              file=sys.stderr)
                        failed = True
                        continue
                except subprocess.CalledProcessError as exc:
                    print(f"FAIL {name} {width}x{height} font={font_size}: Chrome {exc}",
                          file=sys.stderr)
                    failed = True
                    continue
                audit_note = ""
                if not args.skip_layout_audit:
                    match = re.search(r'<script id="layout-audit-result" type="application/json">(.*?)</script>',
                                      completed.stdout, re.DOTALL)
                    if match is None:
                        print(f"FAIL {name} {width}x{height} font={font_size}: layout audit did not run",
                              file=sys.stderr)
                        failed = True
                    else:
                        audit = json.loads(html_lib.unescape(match.group(1)))
                        audit_note = (f" rows={audit['rows']} controls={audit['controls']}"
                                      f" text={audit['text']}")
                        if audit["issues"]:
                            print(f"FAIL {name} {width}x{height} font={font_size}: "
                                  + "; ".join(audit["issues"]), file=sys.stderr)
                            failed = True
                print(f"{name} {width}x{height} font={font_size}:{audit_note} {shot}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
