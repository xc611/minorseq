// Copyright (c) 2016, Pacific Biosciences of California, Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted (subject to the limitations in the
// disclaimer below) provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//  * Neither the name of Pacific Biosciences nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
// GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY PACIFIC
// BIOSCIENCES AND ITS CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL PACIFIC BIOSCIENCES OR ITS
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
// USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
// OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

// Author: Armin Töpfer

#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <boost/optional.hpp>

#include <pacbio/juliet/AminoAcidCaller.h>
#include <pacbio/juliet/AminoAcidTable.h>
#include <pacbio/statistics/Fisher.h>
#include <pbcopper/json/JSON.h>

namespace PacBio {
namespace Juliet {
using AAT = AminoAcidTable;

AminoAcidCaller::AminoAcidCaller(const std::vector<std::shared_ptr<Data::ArrayRead>>& reads,
                                 const ErrorEstimates& error, const TargetConfig& targetConfig)
    : nucMatrix_(reads), error_(error), targetConfig_(targetConfig)
{
    msa_ = std::unique_ptr<Data::MSAByColumn>(new Data::MSAByColumn(reads));

    CallVariants();
}

int AminoAcidCaller::CountNumberOfTests(const std::vector<TargetGene>& genes) const
{
    int numberOfTests = 0;
    for (const auto& gene : genes) {
        for (int i = gene.begin; i < gene.end - 2; ++i) {
            // Relative to gene begin
            const int ri = i - gene.begin;
            // Only work on beginnings of a codon
            if (ri % 3 != 0) continue;
            // Relative to window begin
            const int bi = i - nucMatrix_.BeginPos;

            std::unordered_map<std::string, int> codons;
            for (const auto& nucRow : nucMatrix_.Matrix) {
                const auto& row = nucRow.Bases;
                // Read does not cover codon
                if (bi + 2 >= static_cast<int>(row.size()) || bi < 0) continue;
                if (row.at(bi + 0) == ' ' || row.at(bi + 1) == ' ' || row.at(bi + 2) == ' ')
                    continue;

                // Read has a deletion
                if (row.at(bi + 0) == '-' || row.at(bi + 1) == '-' || row.at(bi + 2) == '-')
                    continue;

                std::string codon = std::string() + row.at(bi) + row.at(bi + 1) + row.at(bi + 2);

                // Codon is bogus
                if (AAT::FromCodon.find(codon) == AAT::FromCodon.cend()) continue;

                codons[codon]++;
            }
            numberOfTests += codons.size();
        }
    }
    return numberOfTests;
}

std::string AminoAcidCaller::FindDRMs(const std::string& geneName,
                                      const std::vector<TargetGene>& genes,
                                      const int position) const
{
    std::string drmSummary;
    for (const auto& gene : genes) {
        if (geneName == gene.name) {
            for (const auto& drms : gene.drms) {
                if (std::find(drms.positions.cbegin(), drms.positions.cend(), position) !=
                    drms.positions.cend()) {
                    if (!drmSummary.empty()) drmSummary += " + ";
                    drmSummary += drms.name;
                }
            }
            break;
        }
    }
    return drmSummary;
};
void AminoAcidCaller::CallVariants()
{
    auto genes = targetConfig_.targetGenes;
    const size_t numExpectedMinors = targetConfig_.NumExpectedMinors();
    const bool hasExpectedMinors = numExpectedMinors > 0;

    const bool hasReference = !targetConfig_.referenceSequence.empty();
    // If no user config has been provided, use complete input region
    if (genes.empty()) {
        TargetGene tg(nucMatrix_.BeginPos, nucMatrix_.EndPos, "unknown", {});
        genes.emplace_back(tg);
    }

    VariantGene curVariantGene;
    std::string geneName;
    int geneOffset = 0;

    const auto SetNewGene = [this, &geneName, &curVariantGene, &geneOffset](
        const int begin, const std::string& name) {
        geneName = name;
        if (!curVariantGene.relPositionToVariant.empty())
            variantGenes_.push_back(std::move(curVariantGene));
        curVariantGene = VariantGene();
        curVariantGene.geneName = name;
        geneOffset = begin;
    };

    const auto CodonProbability = [this](const std::string& a, const std::string& b) {
        double p = 1;
        for (int i = 0; i < 3; ++i) {
            if (a[i] == '-' || b[i] == '-')
                p *= error_.deletion;
            else if (a[i] != b[i])
                p *= error_.substitution;
            else
                p *= error_.match;
        }
        return p;
    };

    const int numberOfTests = CountNumberOfTests(genes);

    double truePositives = 0;
    double falsePositives = 0;
    double falseNegative = 0;
    double trueNegative = 0;
    auto MeasurePerformance = [&truePositives, &falsePositives, &falseNegative, &trueNegative, this,
                               &geneName](
        const TargetGene& tg, const std::pair<std::string, int>& codon_counts, const int& codonPos,
        const int& i, const double& p, const int& coverage) {
        const char aminoacid = AAT::FromCodon.at(codon_counts.first);
        auto Predictor = [&tg, &codonPos, &aminoacid, &codon_counts]() {
            if (!tg.minors.empty()) {
                for (const auto& minor : tg.minors) {
                    if (codonPos == minor.position && aminoacid == minor.aminoacid[0] &&
                        codon_counts.first == minor.codon) {
                        return true;
                    }
                }
            }
            return false;
        };
        double relativeCoverage = 1.0 * codon_counts.second / coverage;
        const bool variableSite = relativeCoverage < 0.8;
        if (variableSite) {
            if (p < alpha) {
                if (Predictor())
                    ++truePositives;
                else
                    ++falsePositives;
            } else {
                if (Predictor())
                    ++falseNegative;
                else
                    ++trueNegative;
            }
        }

        return variableSite;
    };

    for (const auto& gene : genes) {
        SetNewGene(gene.begin, gene.name);
        for (int i = gene.begin; i < gene.end - 2; ++i) {
            // Absolute reference position
            const int ai = i - 1;
            // Relative to gene begin
            const int ri = i - geneOffset;
            // Only work on beginnings of a codon
            if (ri % 3 != 0) continue;
            // Relative to window begin
            const int bi = i - nucMatrix_.BeginPos;

            const int codonPos = 1 + (ri / 3);
            auto& curVariantPosition = curVariantGene.relPositionToVariant[codonPos];

            std::map<std::string, int> codons;
            int coverage = 0;
            for (const auto& nucRow : nucMatrix_.Matrix) {
                const auto& row = nucRow.Bases;
                const auto CodonContains = [&row, &bi](const char x) {
                    return (row.at(bi + 0) == x || row.at(bi + 1) == x || row.at(bi + 2) == x);
                };

                // Read does not cover codon
                if (bi + 2 > static_cast<int>(row.size()) || bi < 0) continue;
                if (CodonContains(' ')) continue;
                ++coverage;

                // Read has a deletion
                if (CodonContains('-')) continue;

                const auto codon = std::string() + row.at(bi) + row.at(bi + 1) + row.at(bi + 2);

                // Codon is bogus
                if (AAT::FromCodon.find(codon) == AAT::FromCodon.cend()) continue;

                codons[codon]++;
            }

            if (hasReference) {
                curVariantPosition.refCodon = targetConfig_.referenceSequence.substr(ai, 3);
                if (AAT::FromCodon.find(curVariantPosition.refCodon) == AAT::FromCodon.cend()) {
                    continue;
                }
                curVariantPosition.refAminoAcid = AAT::FromCodon.at(curVariantPosition.refCodon);
            } else {
                int max = -1;
                std::string argmax;
                for (const auto& codon_counts : codons) {
                    if (codon_counts.second > max) {
                        max = codon_counts.second;
                        argmax = codon_counts.first;
                    }
                }
                curVariantPosition.refCodon = argmax;
                if (AAT::FromCodon.find(curVariantPosition.refCodon) == AAT::FromCodon.cend()) {
                    continue;
                }
                curVariantPosition.refAminoAcid = AAT::FromCodon.at(curVariantPosition.refCodon);
            }

            for (const auto& codon_counts : codons) {
                if (AAT::FromCodon.at(codon_counts.first) == curVariantPosition.refAminoAcid)
                    continue;
                double p = (Statistics::Fisher::fisher_exact_tiss(
                                codon_counts.second, coverage,
                                coverage * CodonProbability(curVariantPosition.refCodon,
                                                            codon_counts.first),
                                coverage) *
                            numberOfTests);

                if (p > 1) p = 1;

                const bool variableSite =
                    MeasurePerformance(gene, codon_counts, codonPos, ai, p, coverage);

                if (((hasExpectedMinors && variableSite) || !hasExpectedMinors) && p < alpha) {
                    VariantGene::VariantPosition::VariantCodon curVariantCodon;
                    curVariantCodon.codon = codon_counts.first;
                    curVariantCodon.frequency = codon_counts.second / static_cast<double>(coverage);
                    curVariantCodon.pValue = p;
                    curVariantCodon.knownDRM = FindDRMs(geneName, genes, codonPos);

                    curVariantPosition.aminoAcidToCodons[AAT::FromCodon.at(codon_counts.first)]
                        .push_back(curVariantCodon);
                }
            }
            if (!curVariantPosition.aminoAcidToCodons.empty()) {
                curVariantPosition.coverage = coverage;
                for (int j = -3; j < 6; ++j) {
                    if (i + j >= nucMatrix_.BeginPos && i + j < nucMatrix_.EndPos) {
                        int abs = ai + j;
                        JSON::Json msaCounts;
                        msaCounts["rel_pos"] = j;
                        msaCounts["abs_pos"] = abs;
                        msaCounts["A"] = (*msa_)[abs][0];
                        msaCounts["C"] = (*msa_)[abs][1];
                        msaCounts["G"] = (*msa_)[abs][2];
                        msaCounts["T"] = (*msa_)[abs][3];
                        msaCounts["-"] = (*msa_)[abs][4];
                        if (hasReference)
                            msaCounts["wt"] =
                                std::string(1, targetConfig_.referenceSequence.at(abs));
                        else
                            msaCounts["wt"] =
                                std::string(1, Data::TagToNucleotide((*msa_)[abs].MaxElement()));
                        curVariantPosition.msa.push_back(msaCounts);
                    }
                }
            }
        }
    }
    if (hasExpectedMinors) {
        double tpr = truePositives / numExpectedMinors;
        double fpr = falsePositives / (numberOfTests - numExpectedMinors);
        double acc = (truePositives + trueNegative) /
                     (truePositives + falsePositives + falseNegative + trueNegative);
        std::cerr << tpr << " " << fpr << " " << numberOfTests << " " << acc << " "
                  << falsePositives << std::endl;
    }
    if (!curVariantGene.relPositionToVariant.empty())
        variantGenes_.push_back(std::move(curVariantGene));
}

JSON::Json AminoAcidCaller::JSON()
{
    using JSON::Json;
    Json root;
    std::vector<Json> genes;
    for (const auto& v : variantGenes_) {
        Json j = v.ToJson();
        if (j.find("variant_positions") != j.cend()) genes.push_back(j);
    }
    root["genes"] = genes;

    return root;
}
}
}  // ::PacBio::Juliet
