/* Shared starter material for the rope, chat attachments and Explorer. */
window.createClipRopeSamples = function (language) {
  "use strict";
  const local = (chinese, english) => language === "en" ? english : chinese;
  const desktopPath = "C:\\Users\\Alex\\Desktop";
  function file(sampleFile, name, type, size, preview) {
    const ext = name.split(".").pop().toUpperCase();
    return {sampleFile, name, ext, type, size, preview, sampleImage: ext === "PNG"};
  }
  function folder(id, name, files) {
    return {id, name, path: desktopPath + "\\" + name, files};
  }
  function fileCard(id, owner, ...files) {
    const members = files.map(f => ({...f, folderId: owner.id, parent: owner.path}));
    return {...window.ClipRopeHistory.asFiles(members), id};
  }
  function note(id, text) {
    return {id, kind: "text", text, status: "complete", capabilities: "Text"};
  }
  function link(id, text) {
    return {...note(id, text), kind: "link", host: new URL(text).host};
  }

  const invitation = local("周五 14:30–15:00\n三楼 · 松木间\n请带上笔记本电脑。", "Friday, 2:30–3:00 pm\nPine Room, third floor\nBring your laptop.");
  const agenda = file("agenda", local("试用会安排.pdf", "Session agenda.pdf"), local("PDF 文档", "PDF document"), "312 KB",
      invitation + local("\n\n14:30 自由试用\n14:45 聊聊哪里不顺手\n15:00 结束", "\n\n2:30 Try it out\n2:45 What got in the way?\n3:00 Wrap up"));
  const feedback = file("feedback", local("反馈记录.md", "Feedback notes.md"), local("Markdown 文档", "Markdown document"), "1 KB",
      local("试用反馈\n我想做什么：\n我停在哪一步：\n我原本以为：", "Session notes\nI wanted to…\nI got stuck at…\nI expected…"));
  const specs = folder("specs", local("试用指南", "Session guides"), [
    agenda,
    file("tasks", local("试用任务.md", "Things to try.md"), local("Markdown 文档", "Markdown document"), "3 KB",
        local("把集合地点发给搭档，再把四份会前材料放进同一个文件夹。做完后，把最费时间的那一步记下来。", "Send your partner the meeting location, then put the four handouts in one folder. Jot down which step took the longest.")),
    file("questions", local("访谈提纲.md", "Interview questions.md"), local("Markdown 文档", "Markdown document"), "2 KB",
        local("你刚才在找什么？\n你先看了哪里？\n如果再做一次，你会从哪里开始？", "What were you looking for?\nWhere did you look first?\nWhere would you start next time?")),
    feedback
  ]);
  const sketch = file("chart-draft", local("耗时图初稿.png", "Timing chart draft.png"), local("PNG 图像", "PNG image"), "640 KB");
  const chart = file("paste-chart", local("粘贴耗时.png", "Paste timing.png"), local("PNG 图像", "PNG image"), "1,284 KB");
  const shots = folder("screenshots", local("测试截图", "Test screenshots"), [
    chart,
    file("first-copy-chart", local("首次复制耗时.png", "First copy timing.png"), local("PNG 图像", "PNG image"), "860 KB"),
    file("repeat-copy-chart", local("连续复制耗时.png", "Repeat copy timing.png"), local("PNG 图像", "PNG image"), "942 KB"),
    sketch
  ]);
  const palette = file("palette", local("纸卡配色.json", "Paper palette.json"), local("JSON 配置文件", "JSON configuration"), "4 KB",
      '{\n  "paper": "#fff8ec",\n  "ink": "#322b25",\n  "accent": "#76461e"\n}');
  const mockups = folder("mockups", local("设计素材", "Design assets"), [
    file("peg-grain", local("木夹纹理.svg", "Wooden peg grain.svg"), local("SVG 矢量图", "SVG image"), "36 KB"),
    file("rope-shadow", local("麻绳阴影.hlsl", "Rope shadows.hlsl"), local("着色器文件", "Shader source"), "9 KB"),
    palette
  ]);
  const cueSheet = file("cue-cards", local("主持人手卡.docx", "Host cue cards.docx"), local("Word 文档", "Word document"), "24 KB",
      local("开场\n“你平时会把刚复制的东西放在哪儿？”\n\n试用时先让大家自己摸索，遇到停顿就记下当时想做的事。", "Opening question\n“Where do you keep things you just copied?”\n\nLet everyone explore. When someone pauses, note what they were trying to do."));
  const roster = file("roster", local("签到名单.xlsx", "Sign-in sheet.xlsx"), local("Excel 工作簿", "Excel workbook"), "18 KB",
      local("姓名　｜　签到　｜　分组\n小陈　｜　待到　｜　第一组\n阿林　｜　待到　｜　第二组", "Name · Attendance · Group\nChen · Expected · 1\nLin · Expected · 2"));
  const slides = file("slides", local("开场投影片.pptx", "Opening slides.pptx"), local("PowerPoint 演示文稿", "PowerPoint presentation"), "480 KB",
      local("第一页　平时怎么找回刚复制的内容？\n第二页　今天的两个小任务\n第三页　哪一步最费时间？", "Slide 1 · How do you find something you just copied?\nSlide 2 · Two things to try\nSlide 3 · Which step took the longest?"));
  const bundle = file("bundle", local("演示素材包.zip", "Demo assets.zip"), local("ZIP 压缩文件", "ZIP archive"), "4,812 KB",
      [chart.name, palette.name].join("\n"));
  const release = folder("release", local("会前材料", "Handouts"), [
    cueSheet, roster, slides, bundle,
    file("app", "ClipRope.exe", local("Windows 应用程序", "Windows application"), "2,140 KB"),
    file("projector", local("投影接线说明.txt", "Projector setup.txt"), local("文本文档", "Text document"), "2 KB", local("松木间的投影线是 HDMI 接口，转接头在讲台左侧抽屉。", "The Pine Room projector uses HDMI. The adapter is in the left drawer of the lectern."))
  ]);
  const folders = [specs, shots, mockups, release];
  const desktop = {id: "desktop", name: local("桌面", "Desktop"), path: desktopPath, files: folders.map(f => ({
    sampleFile: "folder-" + f.id, name: f.name, ext: "DIR", type: local("文件夹", "Folder"), size: "—", child: f.id
  }))};
  const receipt = file("receipt", local("上次那单.txt", "Last tea order.txt"), local("文本文档", "Text document"), "1 KB",
      local("阿林：乌龙奶茶，少冰，三分糖。\n小陈：热柠茶，等凉了喝。", "Lin: oolong milk tea, light ice, 30% sugar.\nChen: hot lemon tea. Let it cool first."));
  desktop.files.push(receipt);
  const folderCard = (id, f) => fileCard(id, desktop, desktop.files.find(item => item.child === f.id));
  const agendaCard = fileCard("card-3", specs, agenda);
  const ropes = [
    {id: "main", name: "main", capacity: 50, cards: [
      {...note("card-1", receipt.preview), width: 248},
      {id: "card-2", kind: "image", image_primary: true, source: {width: 1920, height: 1080},
       status: "complete", capabilities: "Image"},
      agendaCard,
      link("card-4", local("https://learn.microsoft.com/zh-cn/windows/apps/design/", "https://learn.microsoft.com/en-us/windows/apps/design/")),
      folderCard("card-5", mockups),
      {id: "card-6", kind: "rich", notice: local("试用会议程", "Session agenda"), text: agenda.preview,
       status: "partial", capabilities: "HTML · RTF · Partial"},
      {id: "card-7", kind: "unavailable", notice: local("原内容不可用", "Original content unavailable"),
       status: "damaged", capabilities: "Content unavailable"},
      fileCard("card-8", release, cueSheet, roster, slides, bundle),
      note("card-9", feedback.preview),
      fileCard("card-10", release, roster),
      fileCard("card-11", release, bundle),
      fileCard("card-12", release, slides)
    ]},
    {id: "work", name: "work", capacity: 30, cards: [
      note("work-1", local("会前十分钟\n□ 接好投影\n□ 摆好签到表\n□ 给每组留一支笔", "Ten minutes before\n□ Connect the projector\n□ Set out the sign-in sheet\n□ Leave a pen for each group")),
      link("work-2", "https://github.com/microsoft/WindowsAppSDK/releases"),
      fileCard("work-3", mockups, palette),
      folderCard("work-4", shots)
    ]},
    {id: "rebin", name: "ReBin", capacity: null, cards: [
      note("rebin-1", local("千万别扔。背面有字。\n\n背面：正面有字。", "DO NOT THROW AWAY. See other side.\n\nOther side: See front.")),
      fileCard("rebin-2", shots, sketch)
    ]}
  ];

  const me = {sender: "self", author: local("我", "Me"), avatarText: local("我", "Me"), avatarBg: "#07c160"};
  const lin = {sender: "other", author: local("阿林", "Lin"), avatarText: local("林", "L"), avatarBg: "#1877f2"};
  const chen = {sender: "other", author: local("小陈", "Chen"), avatarText: local("陈", "C"), avatarBg: "#2b6cb0"};
  const message = (id, author, text) => ({id, ...author, type: "text", text});
  const sessions = [
    {id: "dev-group", title: local("楼下喝点啥（4）", "Tea downstairs? (4)"), avatarText: local("茶", "T"), avatarBg: "#887342", time: "16:42",
     messages: [
       message("msg-1", lin, local("我上次那杯是少冰还是去冰？", "Did I get light ice or no ice last time?")),
       message("msg-2", me, local("等下，我留了小票。", "Hang on, I kept the receipt.")),
       message("msg-3", chen, local("你连奶茶小票都留？", "You keep tea receipts?")),
       message("msg-4", me, local("嗯，晾着呢。", "Yep. Hung it up to dry."))
     ]},
    {id: "designer", title: local("阿林", "Lin"), avatarText: local("林", "L"), avatarBg: "#1877f2", time: "15:18",
     messages: [
       message("msg-5", lin, local("你见过一张写着‘千万别扔’的纸吗？", "Seen a note that says “DO NOT THROW AWAY”?")),
       message("msg-6", me, local("见过。写得还挺用力。", "Yep. You really dug the pen in.")),
       message("msg-7", lin, local("那你放哪了？", "So where’d you put it?"))
     ]},
    {id: "file-helper", title: local("文件传输助手", "File Transfer"), avatarText: "⇄", avatarBg: "#2ba245", time: "14:06",
     messages: [
       message("msg-8", me, local("下次还点这家", "Ordering here again.")),
       {id: "msg-9", ...me, type: "file",
        fileName: receipt.name, ext: receipt.ext, meta: receipt.type + " · " + receipt.size,
        member: {...receipt, folderId: desktop.id, parent: desktop.path}}
     ]}
  ];
  return {ropes, folders: [...folders, desktop], sessions};
};
