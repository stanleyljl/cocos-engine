# Workspace directories

- This directory (`D:\work\stanleyljl\cocos-engine`) is the engine directory.
- `D:\work\editors\projects\landscape` is the project directory.
- `D:\work\jobs\terrain` contains documentation and demo files.

# Collaboration rules

- Only modify code and ensure it compiles.
- Do not automatically commit or push code unless the user explicitly requests it.

# Required reminders after changes

- After changing engine TypeScript code, explicitly remind the user to restart the editor.
- After changing Effects or project TypeScript code, explicitly remind the user to rebuild the project.
- After changing C++ code, explicitly remind the user to recompile the native code.
- After changing editor extension or importer code, explicitly remind the user to restart the editor and reimport affected assets when necessary. The editor may retain the old importer in memory until restarted.
- When multiple change types are involved, list all required steps in execution order: restart the editor, reimport affected assets when necessary, rebuild the project, then recompile native code.
- Include the applicable reminders in the final response, stating which steps have already been completed and which remain for the user.

# Progress communication

- Before starting tool-assisted work, briefly tell the user what you will investigate or change.
- During ongoing work, provide a concise progress update at least once every 60 seconds. State what is confirmed, what remains uncertain, and what you will do next; do not leave the user waiting silently.
- If a tool, download, or external source stalls, promptly explain the delay and switch to a bounded retry or an alternative approach. Use short, resumable waits so tools do not prevent timely updates.
- Share useful partial findings as soon as they are available, clearly distinguishing verified facts from hypotheses. Do not delay the main explanation solely to chase secondary details or additional sources.
- When the user asks for status, answer immediately before continuing the task. When asked to pause, stop promptly and summarize the current state.
