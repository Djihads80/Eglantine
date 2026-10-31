# Investigação da inicialização do Pokémon Pokopia

## Escopo e evidências

Análise estática do checkout `b40ad217ab829226841cb71f6e4c5cb05977a225`, derivado de `nx2`.
A compilação informada pelo usuário, `390ca75d8b-nx2`, não está disponível como objeto neste
checkout. Não foi feita uma comparação binária com esse executável. Nenhum teste com o jogo
foi realizado. Ambiente relatado: Windows 11, Ryzen 7 5700X, RX 5500 XT, 32 GB, Vulkan.

O fragmento de log estabelece uma sequência, mas não identifica a primeira operação inválida.

- `src/core/hle/service/am/service/self_controller.cpp`: a tabela de `ISelfController` não
  contém 2400 decimal (`0x960`). Há comandos 230 e 240, distintos de 2400, e o comando 2000,
  que devolve uma sessão gráfica NX2 experimental. Isso não estabelece a assinatura de 2400.
- `src/core/hle/service/service.cpp`: comandos ausentes e handlers nulos passam por
  `ReportUnimplementedFunction`. Processos reconhecidos como Switch 2 recebem automaticamente
  um resultado de sucesso vazio, mesmo sem `EDEN_NX2_AUTO_STUB`. Nenhum dado de saída,
  interface ou handle exigido por uma ABI desconhecida é implementado por esse fallback.
- `src/core/hle/service/nx2_gsv.h`: existem sessões gráficas NX2 e vários handlers ainda
  identificados como experimentais/guessed. Não há ligação comprovada entre eles e 2400.
  Outros serviços também usam o número 2400; números IPC pertencem à interface, não são globais.
- `src/core/hle/kernel/svc_results.h` e `src/core/hle/result.h`: `0xca01` é módulo 1,
  descrição 101, `Kernel::ResultInvalidSize`, apresentado como `2001-0101`.
- `IApplicationFunctions::SetTerminateResult` guarda o resultado fornecido pelo programa em
  `m_applet->terminate_result` e retorna sucesso da operação de registro. Não cria `0xca01`
  nem interrompe o programa.
- `src/core/hle/kernel/svc/svc_exception.cpp`: `BreakReason::Panic` é uma solicitação do guest.
  O handler registra o buffer de diagnóstico e o backtrace; não deve ser convertido em sucesso.

A hipótese mais útil é uma validação de tamanho falhando durante a inicialização: várias SVCs
produzem `ResultInvalidSize`. Um dado/handle ausente após um IPC falsamente bem-sucedido também
pode afetar operações seguintes. O fragmento não permite escolher entre essas possibilidades.
Não há evidência nele de que o driver Radeon ou o backend Vulkan seja a causa.

Um ponto concreto a conferir: `SetHeapSize` aceita alinhamento de 2 MiB e rejeita tamanhos de
8 GiB ou mais (`MainMemorySizeMax`), enquanto o fork também possui layout NX2 de 12 GiB.
Isso merece investigação se o log mostrar uma solicitação desse tamanho; o limite não foi
alterado, pois também participa do dimensionamento de estruturas do kernel e a requisição
real do jogo não foi fornecida.

## Alterações

Correção de diagnóstico, não uma implementação do comando 2400 nem uma correção comprovada
da inicialização:

- O resultado IPC é lido no offset registrado por `ResponseBuilder`, limitado ao tamanho
  escrito. A busca anterior por SFCO em 12 palavras podia confundir um handle com o marcador
  ou perder uma resposta de domínio; TIPC assumia resultado na palavra 2 mesmo com handles.
- `EDEN_NX2_IPC_TRACE` registra entrada e resposta de CMIF/TIPC, inclusive fallback e sucesso,
  com comando, tipo, PID, TID e resultado decodificado. Respostas ausentes não contam como sucesso.
- O aviso de fallback identifica serviço/comando e explicita que o sucesso vazio não implementa
  os dados/handles de saída. A política existente de fallback não foi alterada.
- `SetHeapSize` registra tamanho, alinhamento e limite quando rejeita o pedido, mantendo o
  mesmo resultado e as mesmas validações. O trace não apresenta endereço de saída como válido
  quando a chamada falha.
- `SetTerminateResult` identifica o programa e deixa de se anunciar como stub, pois armazena o
  valor recebido. O resultado armazenado não foi alterado.

## Compilar no Windows

Procedimento conferido contra `docs/Deps.md`, `docs/Build.md`, `NX2_NOTES.md` e
`.github/workflows/release-binaries.yml`; não executado no Windows nesta sessão.

Use um caminho curto, por exemplo `C:\dev\xs2-poke`. Instale Visual Studio 2022 com
**Desktop development with C++** e ferramentas x64, CMake 3.31 ou superior, Ninja, Git,
Python 3.10 ou superior, Vulkan SDK (incluindo glslangValidator) e Qt 6 com Qt Charts.
O CI usa Qt 6.8.3 `msvc2022_64`. Use o Developer PowerShell x64 do Visual Studio e ajuste
`C:/Qt/6.8.3/msvc2022_64` para a instalação efetiva. Bibliotecas e dados adicionais são
baixados pelo CMake; mantenha a verificação TLS e os hashes habilitados.

Na raiz da branch local `fix/pokopia-startup-diagnostics`:

```powershell
cmake -S . -B build/pokopia -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 `
  -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_SDL3=ON `
  -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF `
  -DBUILD_TESTING=ON -DYUZU_TESTS=ON -DDYNARMIC_TESTS=OFF `
  -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF
cmake --build build/pokopia --target yuzu yuzu-cmd tests --parallel 4
ctest --test-dir build/pokopia --output-on-failure -R '^tests$'
& .\build\pokopia\bin\tests.exe '[core][hle][ipc]'
& C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe .\build\pokopia\bin\eglantine.exe
```

Pare se qualquer etapa falhar. Os executáveis deste fork são `build/pokopia/bin/eglantine.exe`
(interface Qt) e `eden-cli.exe`, não o nome antigo `eden.exe` citado no guia genérico.
Mantenha também as DLLs não Qt requeridas pelo build e o runtime MSVC instalado.

## Reproduzir e coletar evidências

Use arquivos do jogo e firmware obtidos pelo próprio usuário. Não inclua chaves, firmware,
ROMs ou dumps de memória em relatórios públicos. Preserve os saves e a configuração atual;
compare execuções com a mesma versão do jogo, update, firmware e configuração Vulkan.

No mesmo Developer PowerShell, antes de iniciar a interface:

```powershell
$env:EDEN_NX2_IPC_TRACE = '1'
$env:EDEN_NX2_SVC_TRACE = '1'
$env:EDEN_NX2_MEM_TRACE = '1'
& .\build\pokopia\bin\eglantine.exe
```

Configure o filtro de log em `*:Info` (ou mais detalhado) e reproduza uma vez. Não é necessário
ativar `EDEN_NX2_AUTO_STUB`; o caminho Switch 2 já força esse fallback. Remover essa variável
não o desativa. Não altere o fallback para esconder a falha.

Colete o log desde o carregamento dos módulos até o panic, incluindo:

1. A requisição 2400, `cmd_buf`, tipo de protocolo e resposta efetiva.
2. A primeira resposta IPC `result=0xca01` ou SVC com saída `[0]=0xca01`, correlacionada ao
   PID/TID e aos argumentos da entrada correspondente. Verifique também outros erros anteriores.
3. Qualquer `SetHeapSize rejected`, o `SetTerminateResult`, o `debug_buffer_err_code`/buffer
   e o backtrace completo com endereços e módulos.
4. Versões exatas do jogo/update/firmware, SHA do emulador e versão do driver AMD.

Se nenhum IPC/SVC retornar esse erro, investigue a validação no SDK/guest indicada pelo
backtrace; o valor pode ser produzido ou propagado no userspace. Para implementar 2400,
primeiro são necessários nome/contrato verificáveis, formato da requisição, tamanhos e tipos
de saída, handles e efeitos observados. Não é suficiente registrar um handler que retorne zero.

Depois da coleta, remova as variáveis de trace do terminal, pois podem gerar logs grandes:

```powershell
Remove-Item Env:EDEN_NX2_IPC_TRACE, Env:EDEN_NX2_SVC_TRACE, Env:EDEN_NX2_MEM_TRACE
```

## Validação nesta sessão

Os testes novos foram compilados com GCC 14.2/CMake 3.31.10 em um harness local que utiliza o
Catch2 v3.13.0 baixado e verificado pelo projeto: 2 casos, 9 assertions, todos passaram.
Cobrem CMIF simples, domínio com handles e SFCO fora da janela antiga, handle igual a SFCO,
TIPC com/sem handles, sucesso e resposta ausente/truncada. Foram registrados no target `tests`.
Isso valida a leitura usada pelo diagnóstico, não o serviço completo nem a execução do jogo.

A configuração Linux completa parou no download obrigatório de `nx_tzdb`:
`git.eden-emu.dev` foi negado pelo proxy com HTTP 403. O domínio foi salvo no rascunho do
ambiente, mas a regra ainda não está aplicada. A compilação completa, a suíte integrada,
o executável Windows e Pokémon Pokopia permanecem sem validação nesta sessão.

O pedido explícito do usuário autoriza a criação da branch e de um PR de diagnóstico.
A política `docs/policies/AI.md` pede revisão/testes humanos e divulgação do uso de IA.
O PR deve permanecer em rascunho até a revisão e a validação integrada; seu escopo é a
correção do diagnóstico IPC, sem afirmar que o jogo inicializa.
