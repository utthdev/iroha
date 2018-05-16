/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "validation/impl/stateful_validator_impl.hpp"

#include <boost/range/adaptor/transformed.hpp>

#include "builders/protobuf/proposal.hpp"

namespace iroha {
  namespace validation {

    StatefulValidatorImpl::StatefulValidatorImpl() {
      log_ = logger::log("SFV");
    }

    std::shared_ptr<shared_model::interface::Proposal>
    StatefulValidatorImpl::validate(
        const shared_model::interface::Proposal &proposal,
        ametsuchi::TemporaryWsv &temporaryWsv) {
      log_->info("transactions in proposal: {}",
                 proposal.transactions().size());
      auto checking_transaction = [this](const auto &tx, auto &queries) {
        return bool(queries.getAccount(tx.creatorAccountId()) |
                    [&](const auto &account) {
                      // Check if tx creator has account and has quorum to
                      // execute transaction
                      return boost::size(tx.signatures()) >= account->quorum()
                          ? queries.getSignatories(tx.creatorAccountId())
                          : boost::none;
                    }
                    |
                    [&](const auto &signatories) {
                      // Check if signatures in transaction are account
                      // signatory
                      return this->signaturesSubset(tx.signatures(),
                                                    signatories)
                          ? boost::make_optional(signatories)
                          : boost::none;
                    });
      };

      std::chrono::steady_clock::duration total_time;
      // Filter only valid transactions
      auto filter = [&temporaryWsv, checking_transaction, &total_time](
                        auto &acc, const auto &tx) {
        auto start = std::chrono::steady_clock::now();
        auto answer =
            temporaryWsv.apply(*(tx.operator->()), checking_transaction);
        auto delta = std::chrono::steady_clock::now() - start;
        total_time += delta;

        if (answer) {
          acc.push_back(tx);
        }
        return acc;
      };

      log_->info("filtering");
      auto &txs = proposal.transactions();
      decltype(txs) valid = {};

      auto valid_txs = std::accumulate(txs.begin(), txs.end(), valid, filter);
      log_->info(
          "total answer: {}",
          std::chrono::duration_cast<std::chrono::milliseconds>(total_time).count());
      log_->info("done filtering");
      // TODO: kamilsa IR-1010 20.02.2018 rework validation logic, so that this
      // cast is not needed and stateful validator does not know about the
      // transport
      auto valid_proto_txs =
          valid_txs
          | boost::adaptors::transformed([](const auto &polymorphic_tx) {
              return static_cast<const shared_model::proto::Transaction &>(
                  *polymorphic_tx.operator->());
            });
      log_->info("start building proposal");
      auto validated_proposal = shared_model::proto::ProposalBuilder()
                                    .createdTime(proposal.createdTime())
                                    .height(proposal.height())
                                    .transactions(valid_proto_txs)
                                    .createdTime(proposal.createdTime())
                                    .build();

      log_->info("transactions in verified proposal: {}",
                 validated_proposal.transactions().size());
      return std::make_shared<decltype(validated_proposal)>(
          validated_proposal.getTransport());
    }

    bool StatefulValidatorImpl::signaturesSubset(
        const shared_model::interface::types::SignatureRangeType &signatures,
        const std::vector<shared_model::crypto::PublicKey> &public_keys) {
      // TODO 09/10/17 Lebedev: simplify the subset verification IR-510
      // #goodfirstissue
      std::unordered_set<std::string> txPubkeys;
      for (auto &sign : signatures) {
        txPubkeys.insert(sign.publicKey().toString());
      }
      return std::all_of(public_keys.begin(),
                         public_keys.end(),
                         [&txPubkeys](const auto &public_key) {
                           return txPubkeys.find(public_key.toString())
                               != txPubkeys.end();
                         });
    }

  }  // namespace validation
}  // namespace iroha
