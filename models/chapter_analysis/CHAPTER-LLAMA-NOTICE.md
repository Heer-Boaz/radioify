# Chapter-Llama planner adapters

`chapter-llama-asr-10k-f16.gguf` is a deterministic llama.cpp GGUF conversion
of the Chapter-Llama authors' `asr-10k` PEFT adapter, published under the MIT
license. Radioify uses it to select the candidate timestamps whose frames are
captioned by its pinned MiniCPM-V runtime.

- Source repository: `lucas-ventura/chapter-llama`
- Source revision: `a3837e908f05eb3697873acba2870b2370b1e92d`
- Source file: `outputs/chapterize/Meta-Llama-3.1-8B-Instruct/asr/default/s10k-2_train/default/model_checkpoints/adapter_model.safetensors`
- Converter: llama.cpp `convert_lora_to_gguf.py`
- Converter revision: `b8372eecd94890fd39a59a3a79ab86da1c0db480` (tag `b7146`)
- Output type: `f16`
- SHA-256: `7d160d379d3d654386967bf402f80c09ff24380dd71d54c1317818353a575b05`

`chapter-llama-captions-asr-10k-f16.gguf` is the matching deterministic
conversion of the authors' `captions_asr-10k` PEFT adapter. It consumes the
chronologically interleaved MiniCPM captions and ASR transcript and owns the
final published chapter boundaries and navigation titles.

- Source repository: `lucas-ventura/chapter-llama`
- Source revision: `a3837e908f05eb3697873acba2870b2370b1e92d`
- Source file: `outputs/chapterize/Meta-Llama-3.1-8B-Instruct/captions_asr/asr_s10k-2_train_preds+no-asr-10s/sml10k_train/default/model_checkpoints/adapter_model.safetensors`
- Converter: llama.cpp `convert_lora_to_gguf.py`
- Converter revision: `b8372eecd94890fd39a59a3a79ab86da1c0db480` (tag `b7146`)
- Output type: `f16`
- SHA-256: `4e07a53dfa65e356e691716645d795697427c30dc983b198faf9e84d7eeec6da`

The adapters must be used with Meta Llama 3.1 8B Instruct. The base model is
not redistributed by Radioify; the application downloads its pinned GGUF
artifact after the user confirms model setup, subject to the Llama 3.1
Community License.

## Chapter-Llama license

MIT License

Copyright (c) 2023 Lucas Ventura

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
