#!/usr/bin/env python3
"""chat_template.py -- a chat prompt exactly as the model's own template renders it.

    python chat_template.py "What does PSRAM stand for?"        prints the prompt, \\n-escaped for a board line

The template is tokenizer.chat_template read out of the GGUF itself and rendered with jinja2, the way
transformers and llama.cpp apply it -- one user message, add_generation_prompt on. For Qwen2.5-Coder with no
system message that gives

    <|im_start|>system\\nYou are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>\\n
    <|im_start|>user\\n{message}<|im_end|>\\n<|im_start|>assistant\\n

The system turn is the template's default. Prompts written by hand before 2026-09-27 left it out."""
import struct
import sys

MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
_TPL = None


def template(model=MODEL):
    global _TPL
    if _TPL is not None:
        return _TPL
    width = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
    with open(model, "rb") as f:
        _, _, _, nkv = struct.unpack("<IIQQ", f.read(24))

        def rs():
            n, = struct.unpack("<Q", f.read(8))
            return f.read(n)
        for _ in range(nkv):
            key = rs().decode()
            t, = struct.unpack("<I", f.read(4))
            if t == 8:
                v = rs()
                if key == "tokenizer.chat_template":
                    _TPL = v.decode("utf-8")
                    return _TPL
            elif t == 9:
                at, n = struct.unpack("<IQ", f.read(12))
                if at == 8:
                    for _ in range(n):
                        rs()
                else:
                    f.read(width[at] * n)
            else:
                f.read(width[t])
    raise SystemExit("no tokenizer.chat_template in %s" % model)


def render(user, system=None):
    import jinja2
    env = jinja2.Environment(trim_blocks=False, lstrip_blocks=False)
    env.filters["tojson"] = __import__("json").dumps
    msgs = ([{"role": "system", "content": system}] if system else []) + [{"role": "user", "content": user}]
    return env.from_string(template()).render(messages=msgs, tools=None, add_generation_prompt=True)


def escape(s):
    """The inverse of the board's unescape: backslash first, then newline and tab."""
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


if __name__ == "__main__":
    print(escape(render(sys.argv[1])))
