// Copyright 2022 TF.Text Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_TENSORFLOW_TEXT_CORE_KERNELS_FAST_BERT_NORMALIZER_H_
#define THIRD_PARTY_TENSORFLOW_TEXT_CORE_KERNELS_FAST_BERT_NORMALIZER_H_

#include <cstdint>
#include <vector>

#include "absl/strings/string_view.h"
#include "icu4c/source/common/unicode/utf8.h"
#include "tensorflow/lite/kernels/shim/status_macros.h"
#include "tensorflow_text/core/kernels/darts_clone_trie_wrapper.h"

namespace tensorflow {
namespace text {
namespace text_norm {

// Bit configurations to encode the mapped normalized value. Currently,
// - The 1st bit (from the left) is reserved by Darts-clone Trie.
// - The 2nd bit stores whether the normalized string is different from the
//   codepoint itself. It is also used to differentiate from value 0, which is
//   the value returned by `LookupData()` when the codepoint is not stored on
//   the trie.
// - The next 24 bits (3 to 26) encode the offset of the normalized string in
//   a shared pool.
// - The last 6 bits (27 to 32) encode the length of utf8 bytes of the
//   normalized string.

// The 2rd bit stores whether the normalized string is different from itself.
static constexpr unsigned int kIsNormalizedStringDifferentMask = 0x40000000;

// Number of lowest bits to represent the length of utf8 bytes of mapped
// values. 6-bit is enough to encode the length of the normalized strings.
static constexpr unsigned int kBitsToEncodeUtf8LengthOfNormalizedString = 6;

// The mask for getting the length of the normalized string. It equals to 0x3F
// when `kBitsToEncodeUtf8LengthOfNormalizedString = 6`.
static constexpr unsigned int kNormalizedStringLengthMask =
    (1 << kBitsToEncodeUtf8LengthOfNormalizedString) - 1;

// Maximum length of utf8 bytes of normalized strings. It equals to 63
// when `kBitsToEncodeUtf8LengthOfNormalizedString = 6`.
static constexpr unsigned int kMaximumUtf8LengthOfNormalizedString =
    (1 << kBitsToEncodeUtf8LengthOfNormalizedString) - 1;

// The mask for getting the offset of the normalized string in the pool. It
// equals to 0x3FFFFFC0 when `kBitsToEncodeUtf8LengthOfNormalizedString = 6`.
static constexpr unsigned int kNormalizedStringOffsetMask =
    (kIsNormalizedStringDifferentMask - 1) ^ kNormalizedStringLengthMask;

// Each normalized string is represented as a continuous utf-8 substring in a
// pool. `kMaximumOffsetOfNormalizedString` denotes the maximum offset supported
// here.
static constexpr unsigned int kMaximumOffsetOfNormalizedString =
    (1 << (32 - 2 - kBitsToEncodeUtf8LengthOfNormalizedString)) - 1;

}  // namespace text_norm

// A fast text normalizer for BERT based on codepoint-wise mappings.
class FastBertNormalizer {
 public:
  // Creates an instance.
  //
  // Args:
  //  * trie_data: the pointer to the trie data, which is not owned by this
  //  instance and should be kept alive through the lifetime of the instance.
  //  * data_for_codepoint_zero: the mapped data for the codepoint zero.
  //  * normalized_string_pool: the pointer to the normalized string pool data,
  //  which is not owned by this instance and should be kept alive through the
  //  lifetime of the instance.
  static absl::StatusOr<FastBertNormalizer> Create(
      const uint32_t* trie_data, int data_for_codepoint_zero,
      const char* normalized_string_pool) {
    FastBertNormalizer result;
    SH_ASSIGN_OR_RETURN(auto trie,
                        trie_utils::DartsCloneTrieWrapper::Create(trie_data));
    result.trie_ =
        std::make_unique<trie_utils::DartsCloneTrieWrapper>(std::move(trie));
    result.data_for_codepoint_zero_ = data_for_codepoint_zero;
    result.normalized_string_pool_ =
        reinterpret_cast<const char*>(normalized_string_pool);
    return result;
  }

  // Normalizes the input based on config `lower_case_nfd_strip_accents`.
  //
  // It keeps track that, for each byte in the normalized string, which position
  // in the original input it should best map to (see below notes).
  //
  // Here are a few examples (assuming `lower_case_nfd_strip_accents=true`):
  // * Input: "ABC"
  //   Output: "abc"
  //   Mapping: 0,1,2,3
  //   Explanation: "A" -> "a", "B" -> "b", "C" -> "c". The start position of
  //   "a" maps to position 0 in the input; its exclusive end position equals to
  //   the start position of "b", which maps to position 1 in the input. The
  //   start position of "c" maps to position 2 in the input. The exclusive end
  //   position of "c" (which is also the end of the normalized string) maps to
  //   position 3 in the input (i.e., the end of input).
  // * Input: "B\x41\xCC\x80C"
  //   Output: "bac"
  //   Mapping: 0,1,4,5
  //   Explanation: "\x41\xCC\x80" -> "a". So the start position of "a" maps to
  //   position 1 in the input; the exclusive end position of "a" (which is also
  //   the start position of "c") is position 4 in the input. The exclusive end
  //   position of "c" (which is also the end of the normalized string) maps to
  //   position 5 in the input (i.e., the end of input).
  // * Input: "a\xCE\x89"
  //   Output: "a\xCE\xB7"
  //   Mapping: 0,1,1,3
  //   Explanation: "\xCE\xB9" (2 bytes) -> "\xCE\xB7" (2 bytes). Because
  //   "\xCE\xB7" represents the normalized string of the codepoint U+0389 (i.e.
  //   "\xCE\x90"), their start positions both map to position 1 in the input
  //   (which is the start position of that codepoint).
  // * Input: "a\xC2\xBC"
  //   Output = "a1\xE2\x81\x84""4"
  //   Mapping: 0,1,1,1,1,1,3
  //   Explanation: "\xC2\xBC" (2 bytes) -> "1\xE2\x81\x84""4" (5 bytes). The
  //   start points of those 5 bytes all point to position 1 in the input, which
  //   is the start position of that codepoint.
  //
  // Note that if the input character is not changed after normalization, the
  // bytes are mapped to their original byte locations. For example:
  // * Input: "a\xCC\x80"
  //   Output: "a\xCC\x80"
  //   Mapping: 0,1,2,3
  // However, if a multibyte character is changed after normalization, all bytes
  // of the result character map to the first byte of the character in the
  // input.
  // * Input: "a\xCE\x89"
  //   Output: "a\xCE\xB7"
  //   Mapping: 0,1,1,3
  // The reasons are two-folds:
  // 1. When a multibyte character is changed after normalizatoon, it is not
  // always feasible to map every internal byte in the output back to their
  // corresponding byte in the input. For example, consider the cases where
  // 2-bytes are normalized to 3-bytes or vice versa.
  // 2. The mapping of the internal bytes in the normalized text is usually not
  // used, because users work with UTF-8 output in unit of codepoints, and only
  // the mapping of the first byte is important.
  //
  //
  // This function does not check whether the input is valid utf-8. This
  // behavior is consistent with the existing TF.Text::BertTokenizer.
  //
  // Args:
  //  * input_text: The input text.
  //  * is_output_identical_as_input: True if the normalized string is the
  //  same as the input. In this case, `output_normalized_text` is empty and
  //  `output_normalized_offset_mapping` is not changed.
  //  * output_normalized_text: The normalized text.
  //  * output_normalized_offset_mapping: In addition to the existing content,
  //  the extended new content has size 1 plus the size of `normalized_text`.
  //  Each value is the mapped offset of each byte of `normalized_text` in the
  //  original `input_text`. The final value maps the end of `normalized_text`
  //  to the end of `input_text`.
  template <bool kGetOffsets>
  void NormalizeText(absl::string_view input_text,
                     bool* is_output_identical_as_input,
                     std::string* output_normalized_text,
                     std::vector<int>* output_normalized_offset_mapping) const {
    *output_normalized_text = "";
    // `output_normalized_offset_mapping` is not cleared so the existing content
    // is kept.
    int last_pos_to_copy_over = 0;  // Mark where the copy stopped last time.
    auto copy_unchanged_input_to_output =
        [input_text, output_normalized_text, output_normalized_offset_mapping,
         &last_pos_to_copy_over](int exclusive_copy_end) {
          // Copy from `last_pos_to_copy_over` to `exclusive_copy_end` and
          // update `last_pos_to_copy_over` accordingly.
          if (last_pos_to_copy_over < exclusive_copy_end) {
            absl::StrAppend(
                output_normalized_text,
                input_text.substr(last_pos_to_copy_over,
                                  exclusive_copy_end - last_pos_to_copy_over));
            if constexpr (kGetOffsets) {
              for (int i = last_pos_to_copy_over; i < exclusive_copy_end; ++i) {
                output_normalized_offset_mapping->push_back(i);
              }
            }
            last_pos_to_copy_over = exclusive_copy_end;
          }
        };
    int cur_pos = 0;  // Current position in `input_text` to process.
    while (cur_pos < input_text.size()) {
      int next_pos = cur_pos;
      U8_FWD_1(input_text.data(), next_pos, input_text.size());
      const int cp_byte_length = next_pos - cur_pos;
      if (cp_byte_length == 0) {
        // The codepoint here has length 0, which is probably invalid UTF-8.
        // Copy the remaining unchanged text if any.
        copy_unchanged_input_to_output(cur_pos);
        // Output a whitespace here to replace the invalid UTF-8 byte.
        absl::StrAppend(output_normalized_text, " ");
        if constexpr (kGetOffsets) {
          output_normalized_offset_mapping->push_back(cur_pos);
        }
        // Move by one byte.
        ++cur_pos;
        // Mark the next position to copy over.
        last_pos_to_copy_over = cur_pos;
        continue;
      }
      const int encoded_data =
          LookupData(input_text.substr(cur_pos, cp_byte_length));
      if (!IsNormalizedStringDifferent(encoded_data)) {
        // The codepoint is the same as the normalized. We skip here and copy
        // over in an aggregation way for efficiency reasons.
        cur_pos += cp_byte_length;  // Now move by one codepoint.
        continue;
      }
      absl::string_view normalized_codepoint =
          GetNormalizedString(encoded_data);
      // Copy the previous unchanged text if any.
      copy_unchanged_input_to_output(cur_pos);

      // Output the normalized codepoint text.
      absl::StrAppend(output_normalized_text, normalized_codepoint);
      if constexpr (kGetOffsets) {
        // Every byte of the normalized string should be map to the same start
        // position of the current codepoint in the original `input_text`.
        for (int i = 0; i < normalized_codepoint.size(); ++i) {
          output_normalized_offset_mapping->push_back(cur_pos);
        }
      }
      // Move by one codepoint.
      cur_pos += cp_byte_length;
      // Mark the next position to copy over.
      last_pos_to_copy_over = cur_pos;
    }
    if (last_pos_to_copy_over == 0) {
      // This means that the normalized string would be the same as the input.
      *is_output_identical_as_input = true;
      return;
    }
    *is_output_identical_as_input = false;
    // Copy the remaining unchanged text if any.
    copy_unchanged_input_to_output(input_text.size());
    // Push one more mapping from end_of_normalized to end_of_original.
    if constexpr (kGetOffsets) {
      output_normalized_offset_mapping->push_back(input_text.size());
    }
  }

 private:
  // Use the public Create() method.
  FastBertNormalizer() {}

  // Returns true if the normalized string is different from the codepoint (from
  // the encoded `data`). If `data`==0, it means the normalized string is the
  // same; in that case, this function returns false correctly.
  static bool IsNormalizedStringDifferent(int data) {
    return static_cast<bool>(data &
                             text_norm::kIsNormalizedStringDifferentMask);
  }

  // Calls this only when IsNormalizedStringDifferent(data) returns true.
  absl::string_view GetNormalizedString(int data) const {
    const int len = data & text_norm::kNormalizedStringLengthMask;
    if (!len) {
      return "";
    }
    const int offset = (data & text_norm::kNormalizedStringOffsetMask) >>
                       text_norm::kBitsToEncodeUtf8LengthOfNormalizedString;
    return absl::string_view(normalized_string_pool_ + offset, len);
  }

  // Looks up the character in format of utf8 string format and returns the
  // associated data. If not found, returns 0. Note that 0 also means the
  // normalized string is the same as the codepoint itself (refer to
  // `kIsNormalizedStringDifferentMask`).
  int LookupData(absl::string_view utf8_view) const {
    return LookupData(utf8_view.data(), utf8_view.size());
  }

  // The actual implementation of LookupData(). 'utf8_view_ptr' and 'size'
  // should point to the utf8 view of a codepoint. Performance-critical.
  // Implicitly inline.
  int LookupData(const char* utf8_view_ptr, int size) const {
    // Darts_clone trie cannot encode the empty input string, so we store and
    // return this value separately.
    if (size == 0 || *utf8_view_ptr == '\0') return data_for_codepoint_zero_;
    auto cursor = trie_->CreateTraversalCursorPointToRoot();
    if (!trie_->TryTraverseSeveralSteps(
            cursor, absl::string_view(utf8_view_ptr, size))) {
      return 0;
    }
    int data;
    if (!trie_->TryGetData(cursor, data)) {
      return 0;
    }
    return data;
  }

  // Provides traversal/data-accessing methods on the trie. It has a pointer
  // that points to 'trie_array_'.
  std::unique_ptr<trie_utils::DartsCloneTrieWrapper> trie_;

  // The encoded data for the special codepoint '\0'. Darts_clone trie cannot
  // encode the empty string, so we store this value separately.
  int data_for_codepoint_zero_;

  // The string pool of normalized strings. Each normalized string is a
  // substring denoted by (offset and length).
  const char* normalized_string_pool_;
};

}  // namespace text
}  // namespace tensorflow

#endif  // THIRD_PARTY_TENSORFLOW_TEXT_CORE_KERNELS_FAST_BERT_NORMALIZER_H_