# A versão do firmware (proposta §4.8): a do platformio.ini, seguida do commit, por exemplo
# 0.1.0-a58af37, de -dirty se há mudanças sem commit e do sufixo do ambiente (-sim no esp32-sim,
# proposta §4.7). Entra só nas fontes de src/, para um
# commit novo não recompilar as bibliotecas.
# Origem: firmware_version.py do firmware_telemetria (origin/main).
import subprocess

Import("env", "projenv")  # noqa: F821 (injetados pelo SCons do PlatformIO)


def _git(*args):
    return subprocess.run(["git", *args], cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                          capture_output=True, text=True, check=True).stdout.strip()


versao = env.GetProjectConfig().get("firmware", "versao")  # noqa: F821
try:
    versao += "-" + _git("rev-parse", "--short=7", "HEAD")
    if _git("status", "--porcelain", "--untracked-files=no"):
        versao += "-dirty"
except (OSError, subprocess.CalledProcessError):
    pass
versao += env.GetProjectOption("custom_versao_sufixo", "")  # noqa: F821

projenv.Append(CPPDEFINES=[("VGUARD_VERSAO_FIRMWARE", env.StringifyMacro(versao))])  # noqa: F821
