#!/usr/bin/env python3
"""Проигрывает прохождения heart в браузере и проверяет, что каждая концовка достижима.

Маршруты берёт из walk.py: тот же разбор скрипта, тот же исполнитель правил. Python
считает, каким по порядку чипом является каждое слово команды, а браузер находит
границы чипов по пикселям — тогда не нужны метрики шрифта, и драйвер не врётся из-за
того, что в нём другие цифры.

  python3 tools/py/play_web.py routes.json assets/script/heart.script assets/script
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import walk  # noqa: E402


def gather_pick(w, room, mask, prefix):
    """Чипы в том порядке, в каком их отдаёт интерфейс: game_next плюс фильтр
    «слово продолжает команду или её завершает». Ровно это делает ui_gather_pick."""
    out = []
    for word in w.next_words(room, mask, prefix):
        tail = list(prefix) + [word]
        if w.more_words(room, mask, tail) or w.rule_at(room, mask, tail) >= 0:
            out.append(word)
    return out


def plan_route(w, route):
    """Маршрут -> список команд, каждая команда как список индексов чипов.

    Узел в walk.py хранит комнату после команды, а чипы считаются в комнате
    до неё, поэтому текущая комната берётся из предыдущего узла, а заданная —
    как результат."""
    room = w.rooms[w.sc.start]
    mask = 0
    plan = []
    for room_id, words in route:
        prefix = []
        indexes = []
        for word in words:
            chips = gather_pick(w, room, mask, prefix)
            if word not in chips:
                raise SystemExit("слова %r нет среди чипов в комнате %s: %s"
                                 % (word, room.id, chips))
            indexes.append(chips.index(word))
            prefix.append(word)
        plan.append(indexes)
        idx = w.rule_at(room, mask, words)
        if idx < 0:
            raise SystemExit("команда %s не сработала бы в %s" % (words, room.id))
        rule = room.rules[idx]
        mask = w.apply(room, idx, mask)
        room = w.rooms[room_id]
    return plan


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) < 3:
        print(__doc__)
        return 2
    routes_path, script_path, out_dir = argv[0], argv[1], argv[2]
    only = argv[3].split(",") if len(argv) > 3 else None

    with open(routes_path, encoding="utf-8") as fh:
        routes = json.load(fh)
    if only:
        routes = {k: v for k, v in routes.items() if k in only}
    with open(script_path, encoding="utf-8") as fh:
        sc = walk.parse(fh.read())
    w = walk.World(sc)

    plans = {}
    for line, route in routes.items():
        plans[line] = plan_route(w, [(r, ws) for r, ws in route])
        print("строка %-4s шагов %2d, команд %2d, слов %3d"
              % (line, len(route), len(plans[line]),
                 sum(len(c) for c in plans[line])))

    with open(os.path.join(out_dir, "plan.json"), "w", encoding="utf-8") as fh:
        json.dump(plans, fh, ensure_ascii=False)
    with open(os.path.join(out_dir, "driver.html"), "w", encoding="utf-8") as fh:
        fh.write(DRIVER)
    print("план записан в %s, драйвер в %s" % (out_dir, out_dir))

    proc = subprocess.run(
        ["chromium", "--headless", "--no-sandbox", "--disable-gpu",
         "--disable-dev-shm-usage", "--user-data-dir=" + tempfile.mkdtemp(prefix="chromep-"),
         "--window-size=1500,1000", "--virtual-time-budget=3000000",
         "--dump-dom", "http://127.0.0.1:8131/driver.html"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180 * 10)
    dom = proc.stdout.decode("utf-8", "replace")
    import re
    m = re.search(r'<pre id="?log"?[^>]*>(.*?)</pre>', dom, re.S)
    import html
    print(html.unescape(m.group(1)).strip() if m else "отчёт не найден")
    return 0


DRIVER = r"""<!doctype html>
<meta charset="utf-8">
<body style="margin:0;background:#111">
<pre id="log" style="color:#0f0;font-size:12px;white-space:pre-wrap"></pre>
<iframe id="f" style="border:0;width:1280px;height:900px"></iframe>
<script>
/* Границы чипов ищутся по пикселям: заливка чипа отличается от фона полосы, а между
   чипами фон. Так драйвер не знает ничего про шрифт и не может ошибиться в цифрах,
   которых у него нет. */
function C(){return f.contentDocument.getElementById("canvas")}
function ctx(){return C().getContext("2d", {willReadFrequently:true})}
function H(){var c=C();try{var d=ctx().getImageData(0,0,c.width,c.height).data,h=0;
  for(var i=0;i<d.length;i+=4){h=(h*31+d[i]+d[i+1]*3+d[i+2]*7)|0}return h}catch(e){return 0}}
function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}
/* Полоса command читается одним вызовом: построчный getImageData звал браузер
   десятки раз на каждый опрос, и на длинных маршрутах это была вся стоимость прогона. */
var Y0=296, Y1=478, STRIP=null;
function strip(){
  if(STRIP) return STRIP;
  var c=C();
  STRIP=ctx().getImageData(0,Y0,c.width,Y1-Y0);
  return STRIP;
}
function px(x,y){
  var d=strip(), o=((y-Y0)*d.width+x)*4;
  return d.data[o]+","+d.data[o+1]+","+d.data[o+2];
}
function runs(y){
  var c=C(), d=strip(), i, m;
  /* фон берётся у правого поля, а не как самый частый цвет: чипы занимают больше
     половины строки, и модальным становится уже сам чип */
  var bg=px(634,y);
  var out=[], st=-1;
  for(i=0;i<=634;i++){
    m=px(i,y);
    if(m!==bg){ if(st<0) st=i; }
    else if(st>=0){ if(i-st>=10) out.push([st,i-1]); st=-1; }
  }
  if(st>=0) out.push([st,633]);
  return out;
}
/* Полосы строк, где что-то нарисовано, сгруппированные по соседним строкам. */
function bands(){
  var out=[], cur=null, y;
  for(y=Y0;y<=Y1;y+=4){
    STRIP=null;
    if(runs(y).length>0){
      if(cur===null) cur={y0:y,y1:y};
      else cur.y1=y;
    } else if(cur!==null){ out.push(cur); cur=null; }
  }
  if(cur!==null) out.push(cur);
  STRIP=null;
  return out;
}
/* Строка чипов, а не слоты команды и не полоса предметов.
   Раскладка задаёт это жёстко: слоты стоят ровно на 44 (TILE_H + CMD_PAD) выше
   первой строки чипов, а строки чипов идут через 48 (CHIP_H + CHIP_GAP). Правила
   «у кого больше прямоугольников» недостаточно: у слотов их иногда больше, чем у
   чипов, и клик уходил в слоты. */
function chipBands(){
  var b=bands().filter(function(x){return x.y0<452;});
  var chips=[], i;
  for(i=0;i+1<b.length;i++){
    if(Math.abs(b[i+1].y0-b[i].y0-44)<=6){ chips.push(b[i+1]); break; }
  }
  for(i=chips.length?b.indexOf(chips[0])+1:0; chips.length && i<b.length; i++){
    if(Math.abs(b[i].y0-chips[chips.length-1].y0-48)<=6) chips.push(b[i]);
  }
  return chips.map(function(c){
    var yc=Math.round((c.y0+c.y1)/2);
    return {y:yc, runs:runs(yc)};
  });
}
function chipCount(){
  var n=0, cs=chipBands();
  for(var i=0;i<cs.length;i++) n+=cs[i].runs.length;
  return n;
}
function chipAt(idx){
  var cs=chipBands(), acc=0;
  for(var i=0;i<cs.length;i++){
    if(idx<acc+cs[i].runs.length) return {y:cs[i].y, run:cs[i].runs[idx-acc]};
    acc+=cs[i].runs.length;
  }
  return null;
}
/* Пока печатается ответ, чипов на экране нет: игра их прячет, и ждать по кадрам
   бесполезно — под виртуальным временем кадры идут медленно и покой выдаётся раньше
   времени. Поэтому ждём появления нужного числа чипов, а не тишины в кадре. */
async function waitChips(need){
  for(var i=0;i<200;i++){
    if(chipCount()>=need) return true;
    await sleep(200);
  }
  return false;
}
async function click(x,y){
  var c=C(), r=c.getBoundingClientRect(), s=r.width/c.width;
  var cx=r.left+x*s, cy=r.top+y*s;
  for(const t of ["mousedown","mouseup"]){
    c.dispatchEvent(new MouseEvent(t,{clientX:cx,clientY:cy,bubbles:true,cancelable:true,
      view:f.contentWindow,button:0,buttons:t==="mousedown"?1:0,detail:1}));
  }
}
async function play(plan){
  var f=document.getElementById("f");
  f.src="index.html";
  await sleep(2500);
  for(var s=0;s<plan.length;s++){
    var idx=plan[s];
    for(var k=0;k<idx.length;k++){
      var last=(k===idx.length-1);
      if(!await waitChips(idx[k]+1))
        return "шаг "+s+": чипов не дождались (нужен №"+idx[k]+", есть "+chipCount()+")";
      var at=chipAt(idx[k]);
      if(!at) return "шаг "+s+": чип №"+idx[k]+" не найден";
      await click((at.run[0]+at.run[1])/2, at.y);
      if(last){
        /* Команда выполнена. Ждать конца печати незачем: тап в описание досачитывает
           ответ — так игрок и делает. Заодно проверяется, что приём работает и в
           браузере, а не только в расчётах. Место выбрано в описании, где нет ни
           чипов, ни кнопок. */
        await sleep(200);
        await click(320,200);
        await sleep(200);
        await waitChips(1);
      } else {
        await sleep(400);
      }
    }
  }
  /* финал: у игры больше нет ни одного действия */
  for(var i=0;i<200 && chipCount()>0; i++) await sleep(500);
  var fail=f.contentDocument.getElementById("fail");
  if(fail && !fail.hidden) return "страница сообщила об ошибке: "+fail.textContent;
  return null;
}
var log=document.getElementById("log"), f=document.getElementById("f"), out=[];
function say(s){out.push(s); log.textContent=out.join("\n");}
window.addEventListener("unhandledrejection",function(e){say("REJ "+e.reason);});
setTimeout(async function(){
  var plan=await (await fetch("plan.json")).json();
  var lines=Object.keys(plan).sort(function(a,b){return a-b;});
  var hashes={}, ok=0, bad=0, dup=[];
  say("прохождений: "+lines.length);
  for(var i=0;i<lines.length;i++){
    var line=lines[i];
    var err=await play(plan[line]);
    var h=H();
    var verdict=err ? ("НЕ ПРОЙДЕНО: "+err) : "пройдено, конечный кадр "+h;
    if(!err){
      ok++;
      for(var k in hashes) if(hashes[k]===h) dup.push(k+"~"+line);
      hashes[line]=h;
    } else bad++;
    say("строка "+line+": шагов "+plan[line].length+" — "+verdict);
  }
  say("итого: пройдено "+ok+", не пройдено "+bad+
      ", различных конечных кадров "+Object.keys(hashes).length+
      (dup.length?(", совпали: "+dup.join(", ")):", совпадений нет"));
},3000);
</script>
"""


if __name__ == "__main__":
    sys.exit(main())
