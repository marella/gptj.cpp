#include "ggml/ggml.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>
#include <thread>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Utils
   */

  struct gptj_params
  {
    int32_t seed = -1; // RNG seed
    int32_t n_threads = std::min(4, (int32_t)std::thread::hardware_concurrency());
    int32_t n_predict = 200; // new tokens to predict

    // sampling parameters
    int32_t top_k = 40;
    float top_p = 0.9f;
    float temp = 0.9f;

    int32_t n_batch = 8; // batch size for prompt processing
  };

  struct gpt_vocab
  {
    using id = int32_t;
    using token = std::string;

    std::map<token, id> token_to_id;
    std::map<id, token> id_to_token;
  };

  std::vector<gpt_vocab::id> gpt_tokenize(const gpt_vocab &vocab, const std::string &text)
  {
    std::vector<std::string> words;

    // first split the text into words
    {
      std::string str = text;
      std::string pat = R"('s|'t|'re|'ve|'m|'ll|'d| ?[[:alpha:]]+| ?[[:digit:]]+| ?[^\s[:alpha:][:digit:]]+|\s+(?!\S)|\s+)";

      std::regex re(pat);
      std::smatch m;

      while (std::regex_search(str, m, re))
      {
        for (auto x : m)
        {
          words.push_back(x);
        }
        str = m.suffix();
      }
    }

    // find the longest tokens that form the words:
    std::vector<gpt_vocab::id> tokens;
    for (const auto &word : words)
    {
      if (word.size() == 0)
        continue;

      int i = 0;
      int n = word.size();
      while (i < n)
      {
        int j = n;
        while (j > i)
        {
          auto it = vocab.token_to_id.find(word.substr(i, j - i));
          if (it != vocab.token_to_id.end())
          {
            tokens.push_back(it->second);
            i = j;
            break;
          }
          --j;
        }
        if (i == n)
        {
          break;
        }
        if (j == i)
        {
          auto sub = word.substr(i, 1);
          if (vocab.token_to_id.find(sub) != vocab.token_to_id.end())
          {
            tokens.push_back(vocab.token_to_id.at(sub));
          }
          else
          {
            fprintf(stderr, "%s: unknown token '%s'\n", __func__, sub.data());
          }
          ++i;
        }
      }
    }

    return tokens;
  }

  gpt_vocab::id gpt_sample_top_k_top_p(
      const gpt_vocab &vocab,
      const float *logits,
      int top_k,
      double top_p,
      double temp,
      std::mt19937 &rng)
  {
    int n_logits = vocab.id_to_token.size();

    std::vector<std::pair<double, gpt_vocab::id>> logits_id;
    logits_id.reserve(n_logits);

    {
      const double scale = 1.0 / temp;
      for (int i = 0; i < n_logits; ++i)
      {
        logits_id.push_back(std::make_pair(logits[i] * scale, i));
      }
    }

    // find the top K tokens
    std::partial_sort(
        logits_id.begin(),
        logits_id.begin() + top_k, logits_id.end(),
        [](const std::pair<double, gpt_vocab::id> &a, const std::pair<double, gpt_vocab::id> &b)
        {
          return a.first > b.first;
        });

    logits_id.resize(top_k);

    double maxl = -INFINITY;
    for (const auto &kv : logits_id)
    {
      maxl = std::max(maxl, kv.first);
    }

    // compute probs for the top K tokens
    std::vector<double> probs;
    probs.reserve(logits_id.size());

    double sum = 0.0;
    for (const auto &kv : logits_id)
    {
      double p = exp(kv.first - maxl);
      probs.push_back(p);
      sum += p;
    }

    // normalize the probs
    for (auto &p : probs)
    {
      p /= sum;
    }

    if (top_p < 1.0f)
    {
      double cumsum = 0.0f;
      for (int i = 0; i < top_k; i++)
      {
        cumsum += probs[i];
        if (cumsum >= top_p)
        {
          top_k = i + 1;
          probs.resize(top_k);
          logits_id.resize(top_k);
          break;
        }
      }

      cumsum = 1.0 / cumsum;
      for (int i = 0; i < (int)probs.size(); i++)
      {
        probs[i] *= cumsum;
      }
    }

    std::discrete_distribution<> dist(probs.begin(), probs.end());
    int idx = dist(rng);

    return logits_id[idx].second;
  }

  // model file types
  enum ggml_ftype
  {
    GGML_FTYPE_UNKNOWN = -1,
    GGML_FTYPE_ALL_F32 = 0,
    GGML_FTYPE_MOSTLY_F16 = 1,           // except 1d tensors
    GGML_FTYPE_MOSTLY_Q4_0 = 2,          // except 1d tensors
    GGML_FTYPE_MOSTLY_Q4_1 = 3,          // except 1d tensors
    GGML_FTYPE_MOSTLY_Q4_1_SOME_F16 = 4, // tok_embeddings.weight and output.weight are F16
    GGML_FTYPE_MOSTLY_Q4_2 = 5,          // except 1d tensors
    GGML_FTYPE_MOSTLY_Q8_0 = 7,          // except 1d tensors
    GGML_FTYPE_MOSTLY_Q5_0 = 8,          // except 1d tensors
    GGML_FTYPE_MOSTLY_Q5_1 = 9,          // except 1d tensors
  };

  enum ggml_type ggml_ftype_to_ggml_type(const enum ggml_ftype ftype)
  {
    ggml_type wtype = GGML_TYPE_COUNT;

    switch (ftype)
    {
    case GGML_FTYPE_ALL_F32:
      wtype = GGML_TYPE_F32;
      break;
    case GGML_FTYPE_MOSTLY_F16:
      wtype = GGML_TYPE_F16;
      break;
    case GGML_FTYPE_MOSTLY_Q4_0:
      wtype = GGML_TYPE_Q4_0;
      break;
    case GGML_FTYPE_MOSTLY_Q4_1:
      wtype = GGML_TYPE_Q4_1;
      break;
    case GGML_FTYPE_MOSTLY_Q4_2:
      wtype = GGML_TYPE_Q4_2;
      break;
    case GGML_FTYPE_MOSTLY_Q5_0:
      wtype = GGML_TYPE_Q5_0;
      break;
    case GGML_FTYPE_MOSTLY_Q5_1:
      wtype = GGML_TYPE_Q5_1;
      break;
    case GGML_FTYPE_MOSTLY_Q8_0:
      wtype = GGML_TYPE_Q8_0;
      break;
    case GGML_FTYPE_UNKNOWN:
      wtype = GGML_TYPE_COUNT;
      break;
    case GGML_FTYPE_MOSTLY_Q4_1_SOME_F16:
      wtype = GGML_TYPE_COUNT;
      break;
    }

    if (wtype == GGML_TYPE_COUNT)
    {
      fprintf(stderr, "%s: invalid model type %d\n", __func__, ftype);
    }

    return wtype;
  }

  /**
   * GPT-J
   */

  // default hparams (GPT-J 6B)
  struct gptj_hparams
  {
    int32_t n_vocab = 50400;
    int32_t n_ctx = 2048;
    int32_t n_embd = 4096;
    int32_t n_head = 16;
    int32_t n_layer = 28;
    int32_t n_rot = 64;
    int32_t ftype = 1;
  };

  struct gptj_layer
  {
    // normalization
    struct ggml_tensor *ln_1_g;
    struct ggml_tensor *ln_1_b;

    // attention
    struct ggml_tensor *c_attn_q_proj_w;
    struct ggml_tensor *c_attn_k_proj_w;
    struct ggml_tensor *c_attn_v_proj_w;

    struct ggml_tensor *c_attn_proj_w;

    // ff
    struct ggml_tensor *c_mlp_fc_w;
    struct ggml_tensor *c_mlp_fc_b;

    struct ggml_tensor *c_mlp_proj_w;
    struct ggml_tensor *c_mlp_proj_b;
  };

  struct gptj_model
  {
    gptj_hparams hparams;

    // normalization
    struct ggml_tensor *ln_f_g;
    struct ggml_tensor *ln_f_b;

    struct ggml_tensor *wte; // position embedding

    struct ggml_tensor *lmh_g; // language model head
    struct ggml_tensor *lmh_b; // language model bias

    std::vector<gptj_layer> layers;

    // key + value memory
    struct ggml_tensor *memory_k;
    struct ggml_tensor *memory_v;

    //
    struct ggml_context *ctx;
    std::map<std::string, struct ggml_tensor *> tensors;
  };

  // load the model's weights from a file
  bool gptj_model_load(const std::string &fname, gptj_model &model, gpt_vocab &vocab)
  {
    auto fin = std::ifstream(fname, std::ios::binary);
    if (!fin)
    {
      fprintf(stderr, "%s: failed to open '%s'\n", __func__, fname.c_str());
      return false;
    }

    // verify magic
    {
      uint32_t magic;
      fin.read((char *)&magic, sizeof(magic));
      if (magic != 0x67676d6c)
      {
        fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__, fname.c_str());
        return false;
      }
    }

    // load hparams
    {
      auto &hparams = model.hparams;

      fin.read((char *)&hparams.n_vocab, sizeof(hparams.n_vocab));
      fin.read((char *)&hparams.n_ctx, sizeof(hparams.n_ctx));
      fin.read((char *)&hparams.n_embd, sizeof(hparams.n_embd));
      fin.read((char *)&hparams.n_head, sizeof(hparams.n_head));
      fin.read((char *)&hparams.n_layer, sizeof(hparams.n_layer));
      fin.read((char *)&hparams.n_rot, sizeof(hparams.n_rot));
      fin.read((char *)&hparams.ftype, sizeof(hparams.ftype));
    }

    // load vocab
    {
      int32_t n_vocab = 0;
      fin.read((char *)&n_vocab, sizeof(n_vocab));

      if (n_vocab != model.hparams.n_vocab)
      {
        fprintf(stderr, "%s: invalid model file '%s' (bad vocab size %d != %d)\n",
                __func__, fname.c_str(), n_vocab, model.hparams.n_vocab);
        return false;
      }

      std::string word;
      for (int i = 0; i < n_vocab; i++)
      {
        uint32_t len;
        fin.read((char *)&len, sizeof(len));

        word.resize(len);
        fin.read((char *)word.data(), len);

        vocab.token_to_id[word] = i;
        vocab.id_to_token[i] = word;
      }
    }

    // for the big tensors, we have the option to store the data in 16-bit floats or quantized
    // in order to save memory and also to speed up the computation
    ggml_type wtype = ggml_ftype_to_ggml_type((ggml_ftype)(model.hparams.ftype));
    if (wtype == GGML_TYPE_COUNT)
    {
      fprintf(stderr, "%s: invalid model file '%s' (bad ftype value %d)\n",
              __func__, fname.c_str(), model.hparams.ftype);
      return false;
    }

    auto &ctx = model.ctx;

    size_t ctx_size = 0;

    {
      const auto &hparams = model.hparams;

      const int n_embd = hparams.n_embd;
      const int n_layer = hparams.n_layer;
      const int n_ctx = hparams.n_ctx;
      const int n_vocab = hparams.n_vocab;

      ctx_size += n_embd * ggml_type_sizef(GGML_TYPE_F32); // ln_f_g
      ctx_size += n_embd * ggml_type_sizef(GGML_TYPE_F32); // ln_f_b

      ctx_size += n_embd * n_vocab * ggml_type_sizef(wtype); // wte

      ctx_size += n_embd * n_vocab * ggml_type_sizef(wtype); // lmh_g
      ctx_size += n_vocab * ggml_type_sizef(GGML_TYPE_F32);  // lmh_b

      ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // ln_1_g
      ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // ln_1_b

      ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_q_proj_w
      ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_k_proj_w
      ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_v_proj_w

      ctx_size += n_layer * (n_embd * n_embd * ggml_type_sizef(wtype)); // c_attn_proj_w

      ctx_size += n_layer * (4 * n_embd * n_embd * ggml_type_sizef(wtype)); // c_mlp_fc_w
      ctx_size += n_layer * (4 * n_embd * ggml_type_sizef(GGML_TYPE_F32));  // c_mlp_fc_b

      ctx_size += n_layer * (4 * n_embd * n_embd * ggml_type_sizef(wtype)); // c_mlp_proj_w
      ctx_size += n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32));      // c_mlp_proj_b

      ctx_size += n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16); // memory_k
      ctx_size += n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16); // memory_v

      ctx_size += (5 + 10 * n_layer) * 256; // object overhead
    }

    // create the ggml context
    {
      struct ggml_init_params params = {
          .mem_size = ctx_size,
          .mem_buffer = NULL,
          .no_alloc = false,
      };

      model.ctx = ggml_init(params);
      if (!model.ctx)
      {
        fprintf(stderr, "%s: ggml_init() failed\n", __func__);
        return false;
      }
    }

    // prepare memory for the weights
    {
      const auto &hparams = model.hparams;

      const int n_embd = hparams.n_embd;
      const int n_layer = hparams.n_layer;
      const int n_ctx = hparams.n_ctx;
      const int n_vocab = hparams.n_vocab;

      model.layers.resize(n_layer);

      model.wte = ggml_new_tensor_2d(ctx, wtype, n_embd, n_vocab);

      model.ln_f_g = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
      model.ln_f_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

      model.lmh_g = ggml_new_tensor_2d(ctx, wtype, n_embd, n_vocab);
      model.lmh_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_vocab);

      // map by name
      model.tensors["transformer.wte.weight"] = model.wte;

      model.tensors["transformer.ln_f.weight"] = model.ln_f_g;
      model.tensors["transformer.ln_f.bias"] = model.ln_f_b;

      model.tensors["lm_head.weight"] = model.lmh_g;
      model.tensors["lm_head.bias"] = model.lmh_b;

      for (int i = 0; i < n_layer; ++i)
      {
        auto &layer = model.layers[i];

        layer.ln_1_g = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
        layer.ln_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

        layer.c_attn_q_proj_w = ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);
        layer.c_attn_k_proj_w = ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);
        layer.c_attn_v_proj_w = ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);

        layer.c_attn_proj_w = ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);

        layer.c_mlp_fc_w = ggml_new_tensor_2d(ctx, wtype, n_embd, 4 * n_embd);
        layer.c_mlp_fc_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4 * n_embd);

        layer.c_mlp_proj_w = ggml_new_tensor_2d(ctx, wtype, 4 * n_embd, n_embd);
        layer.c_mlp_proj_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

        // map by name
        model.tensors["transformer.h." + std::to_string(i) + ".ln_1.weight"] = layer.ln_1_g;
        model.tensors["transformer.h." + std::to_string(i) + ".ln_1.bias"] = layer.ln_1_b;

        model.tensors["transformer.h." + std::to_string(i) + ".attn.q_proj.weight"] = layer.c_attn_q_proj_w;
        model.tensors["transformer.h." + std::to_string(i) + ".attn.k_proj.weight"] = layer.c_attn_k_proj_w;
        model.tensors["transformer.h." + std::to_string(i) + ".attn.v_proj.weight"] = layer.c_attn_v_proj_w;

        model.tensors["transformer.h." + std::to_string(i) + ".attn.out_proj.weight"] = layer.c_attn_proj_w;

        model.tensors["transformer.h." + std::to_string(i) + ".mlp.fc_in.weight"] = layer.c_mlp_fc_w;
        model.tensors["transformer.h." + std::to_string(i) + ".mlp.fc_in.bias"] = layer.c_mlp_fc_b;

        model.tensors["transformer.h." + std::to_string(i) + ".mlp.fc_out.weight"] = layer.c_mlp_proj_w;
        model.tensors["transformer.h." + std::to_string(i) + ".mlp.fc_out.bias"] = layer.c_mlp_proj_b;
      }
    }

    // key + value memory
    {
      const auto &hparams = model.hparams;

      const int n_embd = hparams.n_embd;
      const int n_layer = hparams.n_layer;
      const int n_ctx = hparams.n_ctx;

      const int n_mem = n_layer * n_ctx;
      const int n_elements = n_embd * n_mem;

      model.memory_k = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);
      model.memory_v = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);

      const size_t memory_size = ggml_nbytes(model.memory_k) + ggml_nbytes(model.memory_v);
    }

    // load weights
    {
      int n_tensors = 0;
      size_t total_size = 0;

      while (true)
      {
        int32_t n_dims;
        int32_t length;
        int32_t ttype;

        fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
        fin.read(reinterpret_cast<char *>(&length), sizeof(length));
        fin.read(reinterpret_cast<char *>(&ttype), sizeof(ttype));

        if (fin.eof())
        {
          break;
        }

        int32_t nelements = 1;
        int32_t ne[2] = {1, 1};
        for (int i = 0; i < n_dims; ++i)
        {
          fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
          nelements *= ne[i];
        }

        std::string name(length, 0);
        fin.read(&name[0], length);

        if (model.tensors.find(name.data()) == model.tensors.end())
        {
          fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__, name.data());
          return false;
        }

        auto tensor = model.tensors[name.data()];
        if (ggml_nelements(tensor) != nelements)
        {
          fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n", __func__, name.data());
          return false;
        }

        if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1])
        {
          fprintf(stderr, "%s: tensor '%s' has wrong shape in model file: got [%d, %d], expected [%d, %d]\n",
                  __func__, name.data(), (int)tensor->ne[0], (int)tensor->ne[1], ne[0], ne[1]);
          return false;
        }

        const size_t bpe = ggml_type_size(ggml_type(ttype));

        if ((nelements * bpe) / ggml_blck_size(tensor->type) != ggml_nbytes(tensor))
        {
          fprintf(stderr, "%s: tensor '%s' has wrong size in model file: got %zu, expected %zu\n",
                  __func__, name.data(), ggml_nbytes(tensor), nelements * bpe);
          return false;
        }

        fin.read(reinterpret_cast<char *>(tensor->data), ggml_nbytes(tensor));

        total_size += ggml_nbytes(tensor);
      }
    }

    fin.close();

    return true;
  }

  // evaluate the transformer
  //
  //   - model:     the model
  //   - n_threads: number of threads to use
  //   - n_past:    the context size so far
  //   - embd_inp:  the embeddings of the tokens in the context
  //   - embd_w:    the predicted logits for the next token
  //
  // The GPT-J model requires about 16MB of memory per input token.
  //
  bool gptj_eval(
      const gptj_model &model,
      const int n_threads,
      const int n_past,
      const std::vector<gpt_vocab::id> &embd_inp,
      std::vector<float> &embd_w,
      size_t &mem_per_token)
  {
    const int N = embd_inp.size();

    const auto &hparams = model.hparams;

    const int n_embd = hparams.n_embd;
    const int n_layer = hparams.n_layer;
    const int n_ctx = hparams.n_ctx;
    const int n_head = hparams.n_head;
    const int n_vocab = hparams.n_vocab;
    const int n_rot = hparams.n_rot;

    const int d_key = n_embd / n_head;

    static size_t buf_size = 256u * 1024 * 1024;
    static void *buf = malloc(buf_size);

    if (mem_per_token > 0 && mem_per_token * N > buf_size)
    {
      const size_t buf_size_new = 1.1 * (mem_per_token * N); // add 10% to account for ggml object overhead

      // reallocate
      buf_size = buf_size_new;
      buf = realloc(buf, buf_size);
      if (buf == nullptr)
      {
        fprintf(stderr, "%s: failed to allocate %zu bytes\n", __func__, buf_size);
        return false;
      }
    }

    struct ggml_init_params params = {
        .mem_size = buf_size,
        .mem_buffer = buf,
        .no_alloc = false,
    };

    struct ggml_context *ctx0 = ggml_init(params);
    struct ggml_cgraph gf = {.n_threads = n_threads};

    struct ggml_tensor *embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
    memcpy(embd->data, embd_inp.data(), N * ggml_element_size(embd));

    // wte
    struct ggml_tensor *inpL = ggml_get_rows(ctx0, model.wte, embd);

    for (int il = 0; il < n_layer; ++il)
    {
      struct ggml_tensor *cur;

      // norm
      {
        cur = ggml_norm(ctx0, inpL);

        // cur = ln_1_g*cur + ln_1_b
        cur = ggml_add(ctx0,
                       ggml_mul(ctx0,
                                ggml_repeat(ctx0, model.layers[il].ln_1_g, cur),
                                cur),
                       ggml_repeat(ctx0, model.layers[il].ln_1_b, cur));
      }

      struct ggml_tensor *inpSA = cur;

      // self-attention
      {
        struct ggml_tensor *Qcur = ggml_rope(ctx0, ggml_reshape_3d(ctx0, ggml_mul_mat(ctx0, model.layers[il].c_attn_q_proj_w, cur), n_embd / n_head, n_head, N), n_past, n_rot, 0);
        struct ggml_tensor *Kcur = ggml_rope(ctx0, ggml_reshape_3d(ctx0, ggml_mul_mat(ctx0, model.layers[il].c_attn_k_proj_w, cur), n_embd / n_head, n_head, N), n_past, n_rot, 0);

        // store key and value to memory
        {
          struct ggml_tensor *Vcur = ggml_transpose(ctx0, ggml_mul_mat(ctx0, model.layers[il].c_attn_v_proj_w, cur));

          struct ggml_tensor *k = ggml_view_1d(ctx0, model.memory_k, N * n_embd, (ggml_element_size(model.memory_k) * n_embd) * (il * n_ctx + n_past));
          struct ggml_tensor *v = ggml_view_2d(ctx0, model.memory_v, N, n_embd,
                                               (n_ctx)*ggml_element_size(model.memory_v),
                                               (il * n_ctx) * ggml_element_size(model.memory_v) * n_embd + n_past * ggml_element_size(model.memory_v));

          ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
          ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
        }

        // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0, 2, 1, 3)
        struct ggml_tensor *Q =
            ggml_permute(ctx0,
                         Qcur,
                         0, 2, 1, 3);

        // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1, 3)
        struct ggml_tensor *K =
            ggml_permute(ctx0,
                         ggml_reshape_3d(ctx0,
                                         ggml_view_1d(ctx0, model.memory_k, (n_past + N) * n_embd, il * n_ctx * ggml_element_size(model.memory_k) * n_embd),
                                         n_embd / n_head, n_head, n_past + N),
                         0, 2, 1, 3);

        // K * Q
        struct ggml_tensor *KQ = ggml_mul_mat(ctx0, K, Q);

        // KQ_scaled = KQ / sqrt(n_embd/n_head)
        struct ggml_tensor *KQ_scaled =
            ggml_scale(ctx0,
                       KQ,
                       ggml_new_f32(ctx0, 1.0f / sqrt(float(n_embd) / n_head)));

        // KQ_masked = mask_past(KQ_scaled)
        struct ggml_tensor *KQ_masked = ggml_diag_mask_inf(ctx0, KQ_scaled, n_past);

        // KQ = soft_max(KQ_masked)
        struct ggml_tensor *KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

        // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1, 2, 0, 3).contiguous()
        struct ggml_tensor *V =
            ggml_view_3d(ctx0, model.memory_v,
                         n_past + N, n_embd / n_head, n_head,
                         n_ctx * ggml_element_size(model.memory_v),
                         n_ctx * ggml_element_size(model.memory_v) * n_embd / n_head,
                         il * n_ctx * ggml_element_size(model.memory_v) * n_embd);

        // KQV = transpose(V) * KQ_soft_max
        struct ggml_tensor *KQV = ggml_mul_mat(ctx0, V, KQ_soft_max);

        // KQV_merged = KQV.permute(0, 2, 1, 3)
        struct ggml_tensor *KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

        // cur = KQV_merged.contiguous().view(n_embd, N)
        cur = ggml_cpy(ctx0,
                       KQV_merged,
                       ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));

        // projection (no bias)
        cur = ggml_mul_mat(ctx0,
                           model.layers[il].c_attn_proj_w,
                           cur);
      }

      struct ggml_tensor *inpFF = cur;

      // feed-forward network
      // this is independent of the self-attention result, so it could be done in parallel to the self-attention
      {
        // note here we pass inpSA instead of cur
        cur = ggml_mul_mat(ctx0,
                           model.layers[il].c_mlp_fc_w,
                           inpSA);

        cur = ggml_add(ctx0,
                       ggml_repeat(ctx0, model.layers[il].c_mlp_fc_b, cur),
                       cur);

        // GELU activation
        cur = ggml_gelu(ctx0, cur);

        // projection
        // cur = proj_w*cur + proj_b
        cur = ggml_mul_mat(ctx0,
                           model.layers[il].c_mlp_proj_w,
                           cur);

        cur = ggml_add(ctx0,
                       ggml_repeat(ctx0, model.layers[il].c_mlp_proj_b, cur),
                       cur);
      }

      // self-attention + FF
      cur = ggml_add(ctx0, cur, inpFF);

      // input for next layer
      inpL = ggml_add(ctx0, cur, inpL);
    }

    // norm
    {
      inpL = ggml_norm(ctx0, inpL);

      // inpL = ln_f_g*inpL + ln_f_b
      inpL = ggml_add(ctx0,
                      ggml_mul(ctx0,
                               ggml_repeat(ctx0, model.ln_f_g, inpL),
                               inpL),
                      ggml_repeat(ctx0, model.ln_f_b, inpL));
    }

    // lm_head
    {
      inpL = ggml_mul_mat(ctx0, model.lmh_g, inpL);

      inpL = ggml_add(ctx0,
                      ggml_repeat(ctx0, model.lmh_b, inpL),
                      inpL);
    }

    // logits -> probs
    // inpL = ggml_soft_max(ctx0, inpL);

    // run the computation
    ggml_build_forward_expand(&gf, inpL);
    ggml_graph_compute(ctx0, &gf);

    // if (n_past%100 == 0) {
    //     ggml_graph_print   (&gf);
    //     ggml_graph_dump_dot(&gf, NULL, "gpt-2.dot");
    // }

    // embd_w.resize(n_vocab*N);
    // memcpy(embd_w.data(), ggml_get_data(inpL), sizeof(float)*n_vocab*N);

    // return result for just the last token
    embd_w.resize(n_vocab);
    memcpy(embd_w.data(), (float *)ggml_get_data(inpL) + (n_vocab * (N - 1)), sizeof(float) * n_vocab);

    if (mem_per_token == 0)
    {
      mem_per_token = ggml_used_mem(ctx0) / N;
    }

    ggml_free(ctx0);

    return true;
  }

  /**
   * API
   */

  struct gptj_model_context
  {
    gpt_vocab vocab;
    gptj_model model;
  };

  gptj_model_context *gptj_load_model(const char *filename)
  {
    gptj_model_context *ctx = new gptj_model_context;
    if (!gptj_model_load(filename, ctx->model, ctx->vocab))
    {
      delete ctx;
      return nullptr;
    }
    return ctx;
  }

  void gptj_free_model(gptj_model_context *ctx)
  {
    ggml_free(ctx->model.ctx);
    delete ctx;
  }

  bool gptj_generate(gptj_model_context *model_ctx,
                     const char *prompt,
                     gptj_params params,
                     bool (*callback)(const char *token))
  {
    if (params.seed < 0)
    {
      params.seed = time(NULL);
    }
    if (params.n_threads <= 0)
    {
      params.n_threads = std::min(4, (int32_t)std::thread::hardware_concurrency());
    }

    std::mt19937 rng(params.seed);

    gpt_vocab &vocab = model_ctx->vocab;
    gptj_model &model = model_ctx->model;

    int n_past = 0;

    std::vector<float> logits;

    // tokenize the prompt
    std::vector<gpt_vocab::id> embd_inp = ::gpt_tokenize(vocab, prompt);

    params.n_predict = std::min(params.n_predict, model.hparams.n_ctx - (int)embd_inp.size());

    std::vector<gpt_vocab::id> embd;

    // determine the required inference memory per token:
    size_t mem_per_token = 0;
    gptj_eval(model, params.n_threads, 0, {0, 1, 2, 3}, logits, mem_per_token);

    bool processing_input = true;
    for (int i = embd.size(); i < embd_inp.size() + params.n_predict; i++)
    {
      // predict
      if (embd.size() > 0)
      {
        if (!gptj_eval(model, params.n_threads, n_past, embd, logits, mem_per_token))
        {
          fprintf(stderr, "%s: failed to predict\n", __func__);
          return false;
        }
      }

      n_past += embd.size();
      embd.clear();

      if (i >= embd_inp.size())
      {
        processing_input = false;
        // sample next token
        const int top_k = params.top_k;
        const float top_p = params.top_p;
        const float temp = params.temp;

        const int n_vocab = model.hparams.n_vocab;

        gpt_vocab::id id = 0;

        {
          id = gpt_sample_top_k_top_p(vocab, logits.data() + (logits.size() - n_vocab), top_k, top_p, temp, rng);
        }

        // add it to the context
        embd.push_back(id);
      }
      else
      {
        // if here, it means we are still processing the input prompt
        for (int k = i; k < embd_inp.size(); k++)
        {
          embd.push_back(embd_inp[k]);
          if (embd.size() > params.n_batch)
          {
            break;
          }
        }
        i += embd.size() - 1;
      }

      if (!processing_input)
      {
        for (auto id : embd)
        {
          if (id == /* end of text token */ 50256 || !(*callback)(vocab.id_to_token[id].c_str()))
          {
            return true;
          }
        }
      }

      // end of text token
      if (embd.back() == 50256)
      {
        break;
      }
    }

    return true;
  }

  int gptj_num_tokens(gptj_model_context *model_ctx, const char *prompt)
  {
    return gpt_tokenize(model_ctx->vocab, prompt).size();
  }

#ifdef __cplusplus
}
#endif
