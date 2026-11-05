Varredura completa do repositório + pesquisa externa concluídas. Não alterei nada (git limpo, nada escrito). Abaixo está o diagnóstico e o plano de otimização, tudo ancorado em arquivo:linha do código real.
Outlast VR — onde o tempo de frame realmente vai, e o que dá para ganhar
1. Como um frame anda hoje (e onde estão os custos)

game thread ── Hook_CalcSceneView (ol_ue3.cpp:7227) ── 2× o_CalcSceneView (olho L/R)
                                                   └─ OLXR_FrameCommit + OLInteract_Tick (ol_interact.cpp:3123)
render thread ── Hook_GetViewPoint (ol_ue3.cpp:6751) por olho  (pose, lean, IPD)
              ── draws (2× o jogo inteiro) ── hooks por draw (d3d9_proxy.cpp:870-886)
              ── Hook_Present (d3d9_proxy.cpp:901)
                   ├─ PaceToHeadset()      ← espera o worker consumir (ol_xr.cpp:5085)
                   ├─ PublishBackbuffer()  ← StretchRect bb → frame compartilhado D3D9Ex (ol_xr.cpp:2844)
                   ├─ espelho do monitor   (d3d9_proxy.cpp:308, 30 fps)
                   └─ o_Present            (janela, IMMEDIATE)
worker XR ── xrWaitFrame → beginFrame → SyncInput → pose
          ── ConsumeUpload → ExWorkerTake: CopyResource 36 MB (ol_xr.cpp:2902)
          ── BlitHalfToEye ×2 → GammaDraw (shader + CreateSRV + CopyResource) (ol_xr.cpp:3576/3485)
          ── xrEndFrame (até 4 camadas: projeção + flip screen + HUD + focus)

O próprio repo já mediu o gargalo (docs/dev/PROJECT-BASELINE.md:348): efeitos de post-processo não mudam nada — o limite é o render thread submetendo 2× os draw calls. Isso define a estratégia: cortar CPU de render por olho e trabalho redundante no caminho de entrega, não mexer em qualidade de imagem.
2. O que já está otimizado (para não repetir)
Já feito	Onde	Ganho medido
D3D9Ex + frames compartilhados (sem readback)	d3d9_proxy.cpp:1285	46 → 74-81 fps
[TEXSHADOW] sombra de texturas em RAM	d3d9_proxy.cpp:1329	136 freezes de 0.6 s → 0
[MIRRORFPS] monitor a 30 fps	d3d9_proxy.cpp:953	+1.6 ms/frame
[OCCLSKIP] caixas de oclusão não desenhadas, resposta imediata	d3d9_proxy.cpp:1246	+7-12%
[HELDFRAME] não re-blita frame repetido	ol_xr.cpp:~5711	~40% dos frames
Hook SetVertexShaderConstantF removido (14k chamadas/frame)	d3d9_proxy.cpp:1525	devolve ciclos
[LENSFIT] frustum por olho	ol_ue3.cpp:7092	mesmo custo, mais nitidez
[NATIVEEYE] replay desligado (kNativeEyeReplayEnabled=false)	d3d9_proxy.cpp:273	—
3. Oportunidades, por (impacto ÷ risco)
A. Baratas, seguras, ganho quase imediato
#	O que	Onde	Por que dá ganho	Risco
A1	timeBeginPeriod(1) no início, + THREAD_PRIORITY_TIME_CRITICAL e opt-out de EcoQoS (SetThreadInformation(ThreadPowerThrottling)) na thread worker XR, + prioridade opcional do processo	hoje não existe nenhum timeBeginPeriod/SetThreadPriority no projeto (grep vazio); criar em OLXR_Init (ol_xr.cpp:2053) e no LogWriter	PaceToHeadset usa Sleep(1): desde Win10 2004 a resolução de timer é por processo, e sem ela Sleep(1) dorme até ~15.6 ms → estouro do orçamento de 1.25×período e jitter. EcoQoS pode rebaixar a thread do compositor em máquinas com energia gerenciada	baixíssimo
A2	Trocar o spin YieldProcessor() + lock do PaceToHeadset por espera por evento (WaitOnAddress em g_pubSeq)	ol_xr.cpp:5089-5097	hoje, no último 1.5 ms antes do limite, a thread de render do jogo queima CPU girando (~100% de um núcleo) competindo com os draws	baixíssimo
A3	Eliminar o CopyResource de 36 MB por frame em ExWorkerTake (criar SRV por slot e ligar direto no blit) + cachear o SRV que GammaDraw cria a cada chamada	ol_xr.cpp:2902 e 3503 (CreateShaderResourceView por olho por frame, com o comentário "não cachear por ponteiro" — a solução segura é cache por índice de imagem da swapchain, limpo no rebuild, ver DestroyEyeSwapchains)	banda de VRAM: 36 MB/frame ≈ 2.6–3.2 GB/s a 72–90 Hz, mais uma alocação de view no driver por olho/frame	baixo
A4	Snapshot por frame dos flags lidos em cada draw call: OcclDrawSkipped() → OLUE3_OcclBypassOn() (chamada cross-TU), MirrorUiDraw → OLUE3_StereoMode/SplitActive	d3d9_proxy.cpp:866-886, 766	2k–6k draws × 2 olhos = até ~12k chamadas/frame, cada uma com 6–8 leituras volatile e 2–3 chamadas não inlináveis	baixo
A5	Hook_SetRenderTarget faz GetBackBuffer (AddRef/Release) a cada troca de RT	d3d9_proxy.cpp:443-452	cachear o ponteiro do backbuffer (invalidado em Reset)	baixíssimo
A6	HUD do camcorder: só reenviar a faixa dinâmica quando o texto muda	ol_xr.cpp ~3975 (UpdateSubresource 1920×160×4 = 1.2 MB/frame)	upload + reconstrução de glifos todo frame	baixíssimo
A7	OLUE3_ObjScanStep: orçamento adaptativo com QPC (ex.: parar em 0,3 ms) em vez de 4000 objetos fixos/frame	ol_ue3.cpp:2111, 2156	varredura SEH-guarded de 4000 objetos todo frame no game thread	baixo
A8	Tirar o espelho do monitor do render thread (desenhar no bridge D3D11, que já tem o frame)	d3d9_proxy.cpp:308, 974	2 StretchRect de frame inteiro no render thread, e para de sujar o backbuffer que é compartilhado	baixo
B. Ganhos médios — precisam de A/B no headset nulo (o protocolo do repo já existe)
#	O que	Detalhe / como validar
B1	Oclusão por olho em vez de "tudo visível"	Hoje OcclForce=1 + OcclSkip=1 troca culling real por "sempre visível". O doc registra que desligar OcclForce rende +2-12% (PROJECT-BASELINE.md:349). A causa raiz está descrita em d3d9_proxy.cpp:1217: o pool de queries é por cena e reciclado por frame, então o 2º olho lê respostas de outras queries. Corrigir de verdade = mapear query→(olho, índice) no Hook_CreateQuery/Hook_QueryGetData, mantendo resposta de frame anterior, sem wait=TRUE. Resultado esperado: os +7-12% do OcclSkip e o culling de volta. A/B: `occlskip 0
B2	PVS do 2º olho reaproveita o do 1º	Hook_PvsGet (ol_ue3.cpp:6947) já está no lugar; cachear por frame o resultado do olho L e devolvê-lo ao R
B3	Fazer os blits dos olhos antes do xrWaitFrame	A captura já foi movida para fora do deadline por esse motivo (ol_xr.cpp:5140) — os blits (BlitHalfToEye) ainda rodam dentro da janela do compositor (ol_xr.cpp:5770+). Abrir a janela tende a reduzir frame perdido/ASW
B4	Swapchain única com arraySize=2	Recomendação oficial (MS OpenXR best practices: "avoid a separate swapchain per eye / double-wide — runtime post-processing"). Hoje são 2 swapchains (ol_xr.cpp:3113-3130). Ganho depende do runtime: medir em VDXR, SteamVR e Link
B5	Menos camadas de composição	Em jogo com camcorder na mão: projeção + flip screen + HUD + focus = 4 camadas (ol_xr.cpp:6154-6196). Desenhar HUD/focus dentro da imagem do olho (o blit já existe) elimina trabalho do compositor por camada — testar visualmente a perda de ajuste de profundidade antes
B6	FSR1 (EASU + RCAS) no blit D3D11	O blit já é um PS 4.0 com LUT de gamma e CAS caseiro (ol_xr.cpp:3233). Trocar o bicúbico+sharpen por EASU+RCAS (HLSL puro, MIT, compatível com GPL-3.0 como o ImGui/MinHook) permite RenderScale 0.70-0.85 com qualidade melhor → ganho real de GPU justamente para quem é GPU-bound (Quest 3 a 1.0 = 9.1 MP/frame)
B7	Perfil "Performance" no launcher	Expor por ConsoleCommand (já existe o canal con, PROJECT-BASELINE.md:350): DynamicShadows (+6% medido), MaxDrawDistanceScale, Decals, ParticleLODBias, com restore ao sair e um ini por trás (regra da casa)
B8	Render direto na imagem da swapchain (RTV typeless)	Elimina g_blitTmp + CopyResource (ol_xr.cpp:3547) = mais 36 MB/frame. Houve um caso antigo de imagem embaralhada; a hipótese mais provável é view sRGB errada — testar só como experimento isolado, com fallback automático
C. Estrutural — maior ganho, exige engenharia reversa (spike de pesquisa)
#	O que	Por que é o maior ganho disponível
C1	Reaproveitar a visibilidade do olho L no olho R (ComputeViewVisibility): copiar o PrimitiveVisibilityMap + listas de dynamic elements	Os dois olhos estão a ~3 cm: o conjunto visível é praticamente idêntico. Pular o culling do 2º olho corta cerca de metade do CPU de visibilidade por frame. Primeiro passo: instrumentar (QPC) o custo de ComputeViewVisibility por olho e imprimir em [PERF] — sem número, não se mexe
C2	Passo intermediário de C1: frustum união (L ∪ R) nos dois passes	Técnica clássica do multi-pass (Unity "multi-pass otimizado"): garante conjuntos idênticos → torna C1 trivialmente válido; sozinho não economiza CPU
C3	Sombras dinâmicas uma vez por frame (compartilhar shadow maps entre olhos)	DynamicShadows off = +6%: é custo puro duplicado por olho
C4	Latência do pose	Já há predição de 1 período e o commit b6a9c16 reverteu PoseLeadPeriods=2 — não repetir sem dados novos (só faz sentido com blits fora do deadline, B3)
4. Plano de medição (sem isso, todo o resto é chute)

O repo já tem [PERF] (frame/present/grab/copy/pace/upload), [XRTIMING], [HITCH], [EXSTAT] e o headset nulo do SteamVR. Faltam três números:

    GPU x CPU: present avg/max já indica GPU-bound (Present bloqueante), mas falta um D3D11_QUERY_TIMESTAMP_DISJOINT em volta dos blits → custo GPU real do bridge (2 olhos + copies).
    Custo por etapa do render thread: contador de tempo dentro de Hook_CalcSceneView (2 vistas) vs. resto do frame.
    Draw calls por frame (g_uiSeen/g_occlSkippedDraws dão uma pista; um contador dedicado em Hook_DrawIndexedPrimitive fecha a conta) — é a métrica que valida A4, B1, C1.

Protocolo: headset nulo, PaceToHeadset=0 + MaxSmoothedFrameRate=300 (como em PROJECT-BASELINE.md:351), 3 pontos fixos do jogo (porta, corredor, tela de teatro), 3 execuções, mediana. Depois confirmar no Quest 3 real com [HITCH].
5. Ordem sugerida de execução

    Instrumentação (§4) — 1 sessão, sem risco.
    A1, A2, A4, A5 — engasgos e CPU por draw, sem tocar em imagem. Validação: [PERF]/[HITCH] antes/depois.
    A3, A6, A7, A8 — banda e CPU desperdiçados.
    B1, B2, B3 — os que mexem em como o jogo desenha → A/B por ini (OcclPerEye=1, PvsShare=1, BlitBeforeWait=1).
    B6 (FSR1) e B7 (perfil de performance) — o que o jogador sente como "mais FPS".
    C1/C2/C3 — só depois dos números, e um por vez (regra da casa: "change one mechanic at a time").

6. O que não vale a pena (já descartado pelos dados do repo)

    Qualidade de post-processo (DOF, SSAO, bloom, light shafts): medido, não muda nada — o render thread é o limite.
    Single-pass stereo / multiview / SPS: exige instancing/vertex amplification e D3D11 no render — impossível com UE3/D3D9 sem reescrever o RHI. O caminho viável é C1 (reutilizar visibilidade), que ataca a mesma conta por outro lado.
    Reativar AllocViewStateRva/SharedStateRva (runtime.ini:25-32): o próprio arquivo avisa para não fazer.
    Submissão de depth buffer (XR_KHR_composition_layer_depth) para reprojeção posicional: exigiria copiar o depth do D3D9 todo frame, e o README manda desligar ASW/motion smoothing neste jogo — ganho não se paga.

7. Referências externas usadas

    Khronos OpenXR Guide — Frame Submission: pareamento obrigatório xrWaitFrame/xrBeginFrame, late update/late latch, e o porquê de submeter a pose exata com que se renderizou (o mod já faz — [FRAMETAG]).
    Microsoft — OpenXR best practices: escolher o primeiro formato de swapchain oferecido, usar uma projeção, evitar camada dupla/swapchain por olho, e o alerta de post-processing do runtime (base de B4/B5).
    MS — Surface sharing between Windows graphics APIs: compartilhamento D3D9Ex↔D3D11 é não sincronizado (o mod já gerencia com event queries) e as restrições de formato/mip/array (por que A8R8G8B8, 1 mip, sem MSAA em ol_xr.cpp:2802).
    VR/AR Wiki — Single-pass stereo rendering: dois passes custam ~2× CPU/driver/geometria; e o truque do multi-pass do Unity (shadow map compartilhado + cull único contra o frustum combinado) → é exatamente o desenho de C1/C2/C3.
    Bruce Dawson — Windows Timer Resolution: The Great Rule Change: desde Win10 2004 timeBeginPeriod é por processo, portanto cada app precisa pedir a resolução de 1 ms (A1).
    MS — SetThreadInformation / ThreadPowerThrottling: THREAD_POWER_THROTTLING_EXECUTION_SPEED = EcoQoS; não deve ser usado em threads de tempo real/frente — daí o opt-out explícito (A1).
    GPUOpen — FidelityFX FSR 1.0: upscaler em pixel shader (EASU + RCAS), MIT — encaixa no blit D3D11 existente (B6).

Quer que eu, na próxima rodada, comece pela instrumentação (§4) ou já pelo pacote A1+A2+A4 (que não mudam um pixel e atacam engasgos)? Se preferir, posso também abrir um docs/PERFORMANCE.md com este plano antes de tocar em código.
adicione mais uma categoria de investigação, caso seja precisa, faça pesquisas na internet 

D. Redução de trabalho duplicado no Unreal Engine 3
- Investigar se existem atualizações de componentes executadas duas vezes por frame.
- Verificar se cálculos de animação e transformações podem ser compartilhados entre as duas vistas.
- Identificar chamadas de funções UE3 que não precisam ser repetidas por olho.
- Investigar atualização de partículas e efeitos que possam estar sendo recalculados desnecessariamente.
Made 11 tool calls

Pesquisa concluída — e ela corrigiu duas coisas do meu plano anterior (uma delas é boa notícia: sombras e partículas não são duplicadas por olho; a outra é que a hipótese do tracker de oclusão já está falsificada neste build). Segue a categoria D, com fontes e com o que o código mostra.
Categoria D — Trabalho duplicado dentro da UE3
D.0 Primeiro: como provar, usando as ferramentas do próprio motor

A Epic documenta explicitamente que, em duas vistas, o custo de render thread dobra, e que as duas métricas que importam são Processed Primitives e Mesh draw calls (Render Thread Profiling and Optimization):

    "The two big rendering thread costs are determining visibility and then submitting draw calls to the GPU. (…) the cost is doubled in split screen!"

E existe um stat feito exatamente para isto — stat splitscreen — que dá processed primitives, mesh draw calls e particle draw calls (Optimizing Content for Splitscreen). O mod já tem o canal para isso: con <comando> (PROJECT-BASELINE.md:350).
Comando a rodar no jogo (via con)	O que responde
stat unit	ms de Game / Draw (render thread) / GPU — diz qual lado é o limite agora
stat splitscreen	processed primitives × mesh draw calls × particle draw calls: quanto do custo é trabalho por vista
stat initviews	tempo e eficácia do culling de visibilidade (o item mais caro por vista)
stat scenerendering	quanto do render thread está em RenderViewFamily
stat sceneupdate	AddLight/AddPrimitive repetidos (a doc da Epic usa esse stat para achar AddLight chamado 10× por frame)
TRACKPARTICLERENDERINGSTATS → DUMPPARTICLERENDERINGSTATS	quais sistemas de partículas custam (dois relatórios na pasta de logs)

Somado a isso, 30 linhas de instrumentação no mod (contadores por frame impressos a cada 5 s, no padrão [PERF] que já existe): CalcSceneView, UpdateSkelPose, GetFovAngle, PvsGet, OcclUpdate, NativePlayerMove, GetViewPoint. Isso responde a pergunta "quantas vezes por frame?" com número, não com teoria — e é a primeira coisa a fazer da categoria D.
D.1 "Componentes atualizados duas vezes por frame?" — a resposta é quase sempre não
Sistema	Quantas vezes por frame	Evidência
Tick do mundo / componentes / partículas (UParticleSystemComponent::TickComponent)	1×	Update é por componente no game thread (TickGroup=TG_DuringAsyncWork, bSkipUpdateDynamicDataDuringTick nos defaults da classe — fonte UE3)
Animação / UpdateSkelPose (o que o mod hooka em 0x6FFC90)	1× por componente	É o update de game thread (ol_ue3.cpp:2786-2791); CalcSceneView só constrói a FSceneView
CalcSceneView	2× (o mod chama) + N× se houver SceneCapture	ol_ue3.cpp:7307-7315
Culling/visibilidade (ComputeViewVisibility, ProcessVisible(ViewIndex,…))	2×	FSceneRenderer::ProcessVisible(INT ViewIndex, DWORD& ViewVisibilityMap, …) — doc da classe
Base pass / draw calls (RenderDPGBasePassStaticData(DPGIndex, FViewInfo& View))	2×	idem — é o custo medido no repo
Shadow maps (CreateWholeSceneProjectedShadow, GatherShadowsForPrimitives(…, BYTE ValidViews, …))	1× por família	o ValidViews + dependência por vista mostram shadow map compartilhado
Dynamic elements (skeletal: upload de bone palette; partículas: instâncias/LOD)	2× (por vista)	é o que gera draw call por vista
Post-process, fog volumes, translucency, decals, light shafts, distortion	2× (RenderView por vista)	idem

    ⚠️ Correção ao plano anterior (item C3): o "+6%" medido com DynamicShadows off é provavelmente o custo base das sombras, não um segundo render de shadow map por olho. Antes de investir em "sombra 1× por frame", confirme com stat scenerendering — a UE3 já compartilha o shadow map entre vistas.

D.2 O que o código do mod duplica (e dá para cortar já)
#	Achado no código	Por que custa	Ação
D-1	Hook_SkelUpdate → PoseApplyVisible(comp) roda para todo componente esquelético do nível, todo frame (ol_ue3.cpp:5270-5281; o próprio código admite: "every skeletal component in the level passes through the SkelUpdate hook", ol_ue3.cpp:4781)	Antes de sair, faz 5–6 chamadas cross-TU (OLXR_ScreenActive, OLXR_MotionControlsReady, ArmMoveClass, OLInteract_ClimbFinishing, SceneWantsTheater), um InterlockedIncrement e, às vezes, sprintf+log	Testar primeiro comp == g_milesComp / mesh == g_poseMesh (2 leituras) e só então os gates. Ganho proporcional ao nº de NPCs na cena
D-2	Hook_GetViewPoint roda 2×/frame e executa SceneStateRead + captura [NVLIGHT] + matemática de lean/IPD (ol_ue3.cpp:6751-6884)	Metade disso não é por olho	Mover o que é "por frame" para o primeiro passe já existe em parte (g_eyePhase == 1, linha 6871) — estender ao resto
D-3	Hook_GetFovAngle é chamado várias vezes por frame pelo próprio motor (o comentário em ol_ue3.cpp:695 registra "one of those sub-70 calls every frame"); cada chamada refaz atanf/tanf + leitura de estado	Trabalho repetido por consulta, não por olho	Cachear por frame (a modulação é constante dentro do frame; ViewFinderZoom já é lido 1×)
D-4	Hook_SpotSetRotation (0x2CA9F0) aplica a correção de mira da lanterna NV a cada chamada (~8000 medidas no comentário, ol_ue3.cpp:790)	Comparações de rotação + cópia por chamada	Cachear a decisão "esta luz é do jogador" por componente/frame
D-5	CamcorderLiveScreen=1 (LensPublish + SceneCapture2D a 30 fps) → renderiza a cena uma terceira vez	3ª passada completa (alvo pequeno, mas passada inteira)	Manter off (já é o default) e documentar o custo no launcher
D.3 O que é por vista e não dá para compartilhar (para não perder tempo)

    Base pass e todos os passes de geometria (RenderDPG*, translucency, decals, fog, distortion): dependem da câmera. Sem instanced stereo (impossível em D3D9/UE3 sem reescrever o RHI), não existe atalho.
    Bone palette / dynamic elements de skeletal mesh: por vista por construção.
    Único atalho legítimo é o do item C1 (reaproveitar o resultado do culling da vista 1 na vista 2) — e o passo anterior a ele, o frustum união, que a Unity usa no multi-pass: "replaced two per-eye frustum culls with one cull against a combined frustum" (VR/AR Wiki).

D.4 Partículas e efeitos

    Tick compartilhado: a simulação roda 1×/frame no game thread; não há o que deduplicar aí.
    O que dói é draw call: a Epic mede "particle draw calls are ~3× more expensive than static mesh and BSP draw calls", e "mesh emitters generate a draw call per particle" (fonte). Em duas vistas, isso é 2× — e é o tipo de conteúdo mais caro por unidade.
    Alavanca específica de duas vistas que a Epic documenta: MediumDetailSpawnRateScale por emissor (reduz/desliga o emissor no modo Medium detail). Só existe se a Outlast tiver sido autora de splitscreen (provavelmente não — jogo single-player de 2013), então não conte com isso; meça com TRACKPARTICLERENDERINGSTATS antes.
    Onde provavelmente vale: [FLIPSCREEN]/night-vision e os efeitos de fumaça/sangue/chuvas em corredores fechados.

D.5 A alavanca oficial de duas vistas que o mod ainda não usa: DetailMode

Isto é o achado mais acionável da categoria D. A Epic:

    "The next best method is setting a component's DetailMode to High, which will cause it to not be rendered at all in splitscreen. (…) Removing components in this way is the most effective for performance, because it reduces 'Processed primitives' and 'Mesh draw calls'."

Na prática, no jogo: con scale set DetailMode 1 (Medium) → todo mesh marcado como High detail (vegetação, papéis, detritos, decoração, alguns efeitos) deixa de ser processado nas duas vistas. Testável hoje pelo canal con, mensurável com stat splitscreen, reversível, e com o custo visual avaliado em 2 minutos.

Alternativa menos agressiva (e documentada no mesmo texto): MaxDrawDistance agressivo / cull distance — reduz draw calls mas não processed primitives.

Se aprovado como feature, entra como opção [Render] DetailMode no ini + toggle no launcher (regra da casa: comportamento novo atrás de ini, log com tag [DETAILMODE], e restore do valor original ao desligar).
D.6 Duas hipóteses que já estão mortas (não repetir)

Registrando para a categoria D não gastar tempo nelas de novo:

    Tracker de oclusão custom da Outlast (PrimitiveSceneProxyOcclusionTracker, 0x29AEF0): hook instalado, calls = 0 em gameplay, force ON não mudou nada (ol_ue3.cpp:6915-6923). Inclusive a ideia que eu ia sugerir ("cachear a cobertura 1×/frame") cai junto — a função não é chamada.
    Bypass do PVS (GetPrecomputedVisibilityData, 0x8A7810): forçado por janelas inteiras, flicker inalterado (ol_ue3.cpp:6937-6948). O hook continua útil só para o [FRAMETAG].
    O que de fato consertou o flicker de duas vistas foi forçar os resultados das hardware occlusion queries a "visível" (OcclForce + OcclSkip) — e é justamente o item B1 do plano anterior que recupera esse culling sem esperar pela GPU.

D.7 Onde a categoria D entra na ordem de execução

    D.0 — instrumentação + stat splitscreen/stat initviews (1 sessão, zero risco). Antes de tudo.
    D-1, D-2, D-3, D-4 — gates baratos nos hooks que rodam por componente/por consulta (ganho imediato em cenas com NPCs, zero risco de imagem).
    D.5 — DetailMode como opção de performance (medido com stat splitscreen), junto com B7.
    D.4 — só depois do DUMPPARTICLERENDERINGSTATS; se partículas não aparecerem no topo, descartar.
    C1/D.3 — reuso de visibilidade entre olhos (o único grande ganho estrutural que sobrou), com a instrumentação de D.0 medindo o alvo.

Resumo do que mudou com esta rodada de pesquisa: category D ganhou 5 itens novos de código (D-1 a D-5), uma alavanca de engine concreta e documentada pela Epic (DetailMode), um método de medição pronto (stat splitscreen, stat initviews), e duas correções ao plano anterior: sombras/partículas não são recalculadas por olho (cai o C3 como estava), e o tracker de oclusão não pode ser otimizado porque não é chamado.

Quer que eu detalhe agora o passo D.0 (os contadores exatos, onde inserir e o texto de log), ou prefere que eu já prepare o pacote D-1…D-4 (gates nos hooks) como patch revisável?*
Vamos começar a aplicar as otimizações: faça as alterações nessa branch: stable-1.2.2-optimization

OutlastVR — Roadmap de otimização

Fase 1 — Instrumentação e diagnóstico

- Medição de CPU/GPU e frametime.
- Contadores de Draw Calls.
- Profiling de visibilidade e renderização.
- `stat splitscreen`, `stat initviews` e `stat scenerendering`.

Fase 2 — Otimização de CPU

- D-1: `Hook_SkelUpdate`.
- D-2: `Hook_GetViewPoint`.
- D-3: Cache de FOV.
- D-4: Cache da lanterna NV.
- A4: Snapshot de flags.
- A5: Cache do BackBuffer.
- A7: Object Scan adaptativo.
- A6: Atualização inteligente do HUD.
Made 127 tool calls

Pronto. Branch stable-1.2.2-optimization criada, com as Fases 1 e 2 aplicadas, commitada e publicada (2d7e89a).
O que entrou
Fase 1 — instrumentação (sem mudar comportamento)
Onde	O que mede	Aparece no log como
d3d9_proxy.cpp — 4 hooks de draw	draw calls submetidas pelo jogo (2 passes/frame) + as engolidas pelo [OCCLSKIP]	draws N/frame (M occl-skipped)
ol_ue3.cpp — Hook_CalcSceneView	µs dentro dos dois CalcSceneView (custo do mod no game thread)	split N us avg M max
ol_ue3.cpp — PoseCompose/Pvs/Occl/Fov	chamadas por frame de UpdateSkelPose, GetFovAngle, PVS e do tracker	skel N/frame (M solved), fov N/frame (M cached), pvs, occl
ol_xr.cpp [PERF-GPU]	GPU das comandos do próprio mod (blits dos olhos + copies), via par disjoint timestamp lido 1 frame depois com DO_NOTFLUSH	gpu(own cmds) X ms
docs/dev/PERFORMANCE.md	protocolo completo + stat splitscreen / initviews / scenerendering / sceneupdate via con, com os alvos da Epic	—

Nada disso espera pela GPU nem infla o frame: contadores são ++ na thread dona, e o timer de GPU nunca dá flush.
Fase 2 — trabalho removido (cada um com switch em [Perf], default = caminho otimizado)

    A4 DrawFastFlags — 3 chamadas cross-módulo por draw call → 1 snapshot por Present. (o de maior alcance: milhares de draws/frame)
    A5 BackBufferCache — GetBackBuffer (COM, AddRef/Release) em cada troca de render target → comparação de ponteiro; invalidado em Reset e em device novo.
    D1 SkelFastGate — o gate do solver + contador + os três restores globais rodavam para todo componente esquelético do nível; agora sai com uma leitura do ponteiro do mesh, e os restores continuam acontecendo 1× por frame.
    D2 — o vetor de lean é constante no frame e passou a ser compartilhado pelos dois olhos.
    D3 FovCache — o getter é chamado várias vezes por frame; cache indexado nas três entradas (frame, valor do motor, zoom).
    D4 NvLightCache — as luzes do jogador são identificadas uma vez, não a cada SetRotation.
    A6 HudSmart — a faixa de leitura (1920×160 = 1.2 MB) só é redesenhada e enviada quando algum readout muda; imagem de swapchain nunca escrita continua sendo preenchida.
    A7 ObjScanBudgetUs=250 — a varredura de objetos passa a ser limitada por tempo, não por 4000 objetos/frame.

Dois achados no caminho

    Bug real corrigido no A7: se o orçamento cortasse a fatia na última parte da tabela, o passe era publicado incompleto (lista de portas/ledges/ladders pela metade). Agora a finalização testa o cursor, não o fim da fatia — e tests/perf_switches_test.py trava essa linha.
    D-2 completo foi rejeitado de propósito: o corpo por olho do Hook_GetViewPoint custa ~300 ns (2 trig + ViewLeanM + uma dúzia de multiplicações sobre pose congelada). Reescrever a injeção para reaproveitar o bloco inteiro colocaria o head tracking em risco por <1 µs/frame. Só o lean foi compartilhado. Está registrado em docs/dev/PERFORMANCE.md ("Investigated and deliberately NOT changed"), junto com a correção sobre sombras dinâmicas (a UE3 já compartilha o shadow map entre vistas) e o tracker de oclusão (0 chamadas em gameplay).

Validação feita aqui

    tests/: 13/13 OK, incluindo o novo tests/perf_switches_test.py, que (a) compila a regra do orçamento e a exercita, (b) exige que o default do ini e o default do código sejam o caminho otimizado para os 7 switches, (c) impede que o caminho por draw volte a chamar OLUE3_StereoMode/SplitActive/OcclBypassOn, e (d) guarda as proteções do upload do HUD e do timer de GPU.
    Balanceamento de chaves/parênteses/colchetes verificado nos 3 .cpp (0/0/0).
    launcher_resolution_slider_test.py continua sem rodar aqui (precisa de mcs/Mono, indisponível no sandbox) — falha pré-existente, não relacionada.
    O launcher grava o ini chave a chave (WritePrivateProfileString), então a seção [Perf] sobrevive a "salvar ajustes".

Não consigo compilar com MSVC neste sandbox (o build é scripts\build.bat, x64/VS2022). Rode scripts\build.bat antes de testar no headset — se algo não compilar, me mande a saída que eu ajusto.
Como provar o ganho (10 minutos)

    Suba o jogo, rode o mesmo trecho 2× e olhe a nova linha [OLVR][PERF] draws … | split … | skel … | fov … | gpu … a cada 5 s.
    Para cada switch: [Perf] X=0 → reinicia → compara game fps e draws/frame. Um switch que não mostrar diferença no log não deve ficar como promessa no ini.
    Em paralelo, [Debug] DevBridge=1 + con stat splitscreen / con stat initviews para ver processed primitives vs mesh draw calls.

Observação de infraestrutura

O commit precisou de identidade git; configurei apenas neste repositório user.name=Cline / user.email=cline@localhost. Se quiser sua autoria: git commit --amend --reset-author depois de configurar a sua.
Próximo passo sugerido (Fase 3)

Pela ordem de impacto que sobrou: B1 (oclusão por olho no lugar do force-visible — recupera os +2-12% do OcclForce mantendo o OcclSkip), B3 (blits dos olhos antes do xrWaitFrame), B6 (FSR1/EASU+RCAS no blit existente) e D.5/DetailMode como opção de performance. Quer que eu siga por B1 ou por B3?
**********************************************************************
** Visual Studio 2022 Developer Command Prompt v17.14.42
** Copyright (c) 2025 Microsoft Corporation
**********************************************************************
[vcvarsall.bat] Environment initialized for: 'x64'
d3d9_proxy.cpp
ol_xr.cpp
S:\VR\OutlastVR\scripts\..\src\ol_xr.cpp(3649): error C2660: 'OLR_EdgeFill': função não recebe 14 argumentos
S:\VR\OutlastVR\scripts\..\src\ol_xr.cpp(3614): note: consulte a declaração de 'OLR_EdgeFill'
S:\VR\OutlastVR\scripts\..\src\ol_xr.cpp(3649): note: ao tentar corresponder a lista de argumentos '(float *, float *, float *, float *, float, float, float, float, float, float, float *, float *, float *, float *)'
S:\VR\OutlastVR\scripts\..\src\ol_xr.cpp(3834): error C2065: 'g_edgeFillMargin': identificador não declarado
S:\VR\OutlastVR\scripts\..\src\ol_xr.cpp(3832): error C2737: 'fill': objetoconst deve ser inicializado
ol_ue3.cpp
S:\VR\OutlastVR\scripts\..\src\ol_ue3.cpp(4880): error C2039: ' injW': não é um membro de 'PoseGate'
S:\VR\OutlastVR\scripts\..\src\ol_ue3.cpp(4871): note: consulte a declaração de 'PoseGate'
ol_menu.cpp
ol_motion.cpp
ol_devbridge.cpp
Made 22 tool calls

Build corrigido e publicado. Os 4 erros tinham duas origens diferentes:
1. Erro meu (1)
Erro	Causa	Correção
ol_ue3.cpp(4880): C2039: 'injW' não é membro de 'PoseGate'	em PoseGateRead() eu gravei g.inj = g_injW e em seguida comparei g.injW < 0.95f	agora compara g.inj < 0.95f (o mesmo valor, recém-gravado)
2. Erros que já existiam no HEAD (3) — o commit b6a9c16 não compilava

O revert "drop PoseLeadPeriods=2 and EdgeFillMargin" removeu o parâmetro marginPct da definição de OLR_EdgeFill, mas deixou a declaração (linha 3614) e um call site (BlitFitToEyeTan, linha 3834) com a assinatura antiga de 15 argumentos:

C2660: 'OLR_EdgeFill': função não recebe 14 argumentos   (call site vs declaração)
C2065: 'g_edgeFillMargin': identificador não declarado
C2737: 'fill': objeto const deve ser inicializado          (cascata do anterior)

Corrigi conforme o intent declarado daquele revert ("no bought margin, no FOV trade-off"):

    declaração agora tem 14 params, igual à definição;
    o call site de BlitFitToEyeTan perdeu o g_edgeFillMargin;
    corrigi os comentários que ainda descreviam a "margem comprada" (em Init() e acima de BlitHalfToEye);
    corrigi a linha do README.md que mandava o jogador aumentar [Stream] EdgeFillMargin — essa chave não existe mais (documentação quebrada induzia a erro).

3. Teste novo para essa classe de falha

O problema de fundo: o build é MSVC e a suíte de testes roda em Linux, então b6a9c16 passou em 13/13 testes com o build quebrado. Criei tests/call_arity_test.py, que sem compilador verifica:

    declaração × definição: função static/extern "C" forward-declarada no mesmo arquivo tem de concordar no número de parâmetros;
    call sites: para uma lista de funções internas com vários chamadores (OLR_EdgeFill, OLUE3_TakeFrameStats, OLProxy_TakeDrawStats, …), toda chamada passa uma quantidade legal de argumentos (respeitando defaults).

Validei o teste contra o bug real: reintroduzi a declaração antiga temporariamente e ele falhou como esperado —

1) declaration vs definition: FAIL
   src/ol_xr.cpp:3614: OLR_EdgeFill declared with 15 parameters, defined with 14 (definition at line 3726)
exit=1

— e o arquivo foi restaurado (conferido por sha256sum -c). Sem essa prova, seria mais um teste que sempre passa.
Verificações extras que fiz para antecipar o próximo build

    aridade: OLUE3_TakeFrameStats 8/8/8, OLProxy_TakeDrawStats 2/2/2, OLR_EdgeFill 14 em todos os 4 pontos (declaração, definição, 2 call sites + o do held frame);
    ordem de declaração, por arquivo, de todos os ~50 símbolos que introduzi (nenhum uso antes da declaração — foi isso que pegou o g.injW e pegaria um g_edgeFillMargin);
    definições duplicadas: nenhuma (g_skelFastGate/g_fovCacheOn/g_nvCacheOn/g_objScanBudgetUs aparecem 2× porque a segunda é a atribuição em ReadConfig);
    balanceamento {}/()/[] = 0/0/0 nos três .cpp;
    suíte completa: 14/14 OK (os 13 anteriores + call_arity_test.py).

Estado: b393d76 em stable-1.2.2-optimization, pushed. Rode scripts\build.bat de novo — se sobrar algum erro, me manda a saída que eu já sei exatamente onde olhar.

Now I'll validate the A8 assumption end-to-end: that a DXGI bitblt swapchain on the game's D3D9 HWND actually shows its pixels, then hands the window back to D3D9.

Fase 3 implementada, compilada, testada e publicada em stable-1.2.2-optimization (commit b79402e, push OK).

O que foi feito
A1 — timer e prioridade da thread XR (src/ol_xr.cpp, Init/WorkerMain)

timeBeginPeriod(1) por processo (desde o Win10 2004 a resolução é por processo; sem isso o Sleep(1) podia dormir ~15,6 ms e perder o intervalo do display).
Thread XR em THREAD_PRIORITY_TIME_CRITICAL + opt-out de EcoQoS, aplicado antes de entrar no loop.
ABOVE_NORMAL do processo disponível, mas desligado por padrão (afeta as threads do jogo; o ganho real é o boost da worker).
A2 — WaitOnAddress no lugar do spin (PaceToHeadset / ConsumeUpload)

A thread de render bloqueia em g_conSeq (agora volatile uint64_t) em vez de girar com YieldProcessor() no último 1,5 ms do orçamento.
A worker chama WakeByAddressAll depois de soltar g_pubLock e só quando o frame realmente avançou.
O deadline continua teto: se o bridge não responder, o frame é publicado como antes.
A8 — espelho do monitor fora da render thread

A worker desenha o olho esquerdo (ou o SBS cru, se DesktopMirrorOneEye=0) direto numa swapchain DXGI bitblt criada no mesmo HWND do jogo, e apresenta com DXGI_PRESENT_DO_NOT_WAIT, no mesmo DesktopMirrorFps.
A render thread deixa de fazer os dois StretchRect de frame inteiro e o Present da janela; só emite o flush.
Fallback vivo: a render thread só se retira se OLXR_MonitorActive() viu um present recente (400 ms, ou dois intervalos quando o MirrorFps é muito baixo). Sem split, modo não-windowed, janela escondida/minimizada, swapchain recusada, worker parado ou headset dormindo → volta ao caminho antigo no frame seguinte.
A factory vem do adaptador do próprio device D3D11 (o LUID do OpenXR não é necessariamente o adapter 0 — importante nesta máquina, que tem Parsec/VD no meio).
Criação recusada é repetida num timer de 2 s, não a cada frame do monitor.
Verificação
scripts/build.bat → [build] OK, sem warnings.
tests/perf_threading_test.py novo: 4/4, e teste de mutação confirmou que ele falha ao enfraquecer a condição do A8, ao remover o wake do pacer ou ao tirar o backoff da swapchain.
tests/call_arity_test.py passa; as seções Python do perf_switches_test.py também (7 switches antigos continuam coerentes). O restante da suíte exige g++, que não existe nesta máquina — limitação já existente.
A premissa do A8 foi provada de ponta a ponta, não só na API: D3D9 mostra vermelho → swapchain DXGI bitblt no mesmo HWND mostra azul → o próximo Present do D3D9 mostra verde de novo com a swapchain DXGI ainda viva (probe em %TEMP%\opencode\swapshow.cpp).
Switches novos em [Perf] (todos com o caminho otimizado como padrão)
TimerResolution=1, ThreadPriority=1, ProcessPriority=0, PaceWait=1, BridgeMirror=1. Cada um documentado no runtime/outlastvr.ini; docs/dev/PERFORMANCE.md ganhou a tabela da fase 3 e as linhas de log [PERF-A1/A2/A8].

Ressalvas
Falta o teste visual no headset/numa sessão real do jogo: o probe prova o mecanismo, mas não a imagem em si.
Enquanto o A8 estiver ativo, present avg no relatório [PERF] fica 0 — é o objetivo, não regressão.
O monitor agora passa pelo mesmo LUT de gamma/night-vision do headset (antes era o backbuffer cru); com gamma neutro a imagem é idêntica.
você já fez o commit e o push? se sim, obrigado 
Sim, já está tudo feito e sincronizado. ✅

Commit: b79402e — perf: stop the render thread spinning and presenting the monitor
Branch: stable-1.2.2-optimization
Push: feito para origin/stable-1.2.2-optimization (0 commits à frente do upstream, árvore de trabalho limpa)
Pode revisar/testar. Se quiser, o próximo passo natural é a rodada no headset para validar visualmente o A8 e colher os logs [PERF-A1], [PERF-A2] e [PERF-A8].