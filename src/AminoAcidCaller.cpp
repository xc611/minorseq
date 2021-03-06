// Copyright (c) 2016-2017, Pacific Biosciences of California, Inc.
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
#include <iomanip>
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
#include <pacbio/juliet/HaplotypeType.h>
#include <pacbio/statistics/Fisher.h>
#include <pacbio/util/Termcolor.h>
#include <pbcopper/json/JSON.h>

namespace PacBio {
namespace Juliet {
using AAT = AminoAcidTable;

AminoAcidCaller::AminoAcidCaller(const std::vector<std::shared_ptr<Data::ArrayRead>>& reads,
                                 const ErrorEstimates& error, const JulietSettings& settings)
    : msaByRow_(reads)
    , msaByColumn_(msaByRow_)
    , error_(error)
    , targetConfig_(settings.TargetConfigUser)
    , verbose_(settings.Verbose)
    , mergeOutliers_(settings.MergeOutliers)
    , debug_(settings.Debug)
    , drmOnly_(settings.DRMOnly)
    , minimalPerc_(settings.MinimalPerc)
    , maximalPerc_(settings.MaximalPerc)
{

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
            const int bi = i - msaByRow_.BeginPos;

            std::unordered_map<std::string, int> codons;
            for (const auto& nucRow : msaByRow_.Rows) {
                const auto& row = nucRow->Bases;
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
                                      const DMutation curDRM) const
{
    std::string drmSummary;
    for (const auto& gene : genes) {
        if (geneName == gene.name) {
            for (const auto& drms : gene.drms) {

                if (std::find(drms.positions.cbegin(), drms.positions.cend(), curDRM) !=
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

void AminoAcidCaller::PhaseVariants()
{
    std::vector<std::pair<int, std::shared_ptr<VariantGene::VariantPosition>>> variantPositions;
    for (const auto& vg : variantGenes_) {
        for (const auto& pos_vp : vg.relPositionToVariant)
            if (pos_vp.second->IsVariant())
                variantPositions.emplace_back(
                    std::make_pair(vg.geneOffset + pos_vp.first * 3, pos_vp.second));
    }

    if (verbose_) {
        std::cerr << "Variant positions:";
        for (const auto& pos_var : variantPositions)
            std::cerr << " " << pos_var.first;
        std::cerr << std::endl;
    }
    std::vector<std::shared_ptr<Haplotype>> observations;

    auto ExtractRegionFromRow = [this](
        const std::shared_ptr<Data::MSARow>& row,
        const std::pair<int, std::shared_ptr<VariantGene::VariantPosition>>& pos_var, int l,
        int r) {
        std::string codon;
        int local = pos_var.first - msaByRow_.BeginPos - 3;
        for (int i = l; i < r; ++i)
            codon += row->Bases.at(local + i);

        return codon;
    };

    // For each read
    for (const auto& row : msaByRow_.Rows) {

        // Get all codons for this row
        std::vector<std::string> codons;
        uint8_t flag = 0;
        for (const auto& pos_var : variantPositions) {
            std::string codon = ExtractRegionFromRow(row, pos_var, 0, 3);
            if (!pos_var.second->IsHit(codon)) {
                flag |= static_cast<int>(HaplotypeType::OFFTARGET);
            }
            codons.emplace_back(std::move(codon));
        }

        // There are already haplotypes to compare against
        int miss = true;

        // Compare current row to existing haplotypes
        auto CompareHaplotypes = [&miss, &codons,
                                  &row](std::vector<std::shared_ptr<Haplotype>>& haplotypes) {
            for (auto& h : haplotypes) {
                // Don't trust if the number of codons differ.
                // That should only be the case if reads are not full-spanning.
                if (h->Codons.size() != codons.size()) {
                    continue;
                }
                bool same = true;
                for (size_t i = 0; i < codons.size(); ++i) {
                    if (h->Codons.at(i) != codons.at(i)) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    h->Names.push_back(row->Read->Name);
                    miss = false;
                    break;
                }
            }
        };

        CompareHaplotypes(observations);

        // If row could not be collapsed into an existing haplotype
        if (miss) {
            auto h = std::make_shared<Haplotype>();
            h->Names = {row->Read->Name};
            h->SetCodons(std::move(codons));
            h->Flags |= flag;
            observations.emplace_back(std::move(h));
        }
    }

    std::vector<std::shared_ptr<Haplotype>> generators;
    std::vector<std::shared_ptr<Haplotype>> filtered;
    for (auto& h : observations) {
        if (h->Size() < 10) h->Flags |= static_cast<int>(HaplotypeType::LOW_COV);
        if (h->Flags == 0)
            generators.emplace_back(std::move(h));
        else
            filtered.emplace_back(std::move(h));
    }

    // Haplotype comparator, ascending
    auto HaplotypeComp = [](const std::shared_ptr<Haplotype>& a,
                            const std::shared_ptr<Haplotype>& b) { return a->Size() < b->Size(); };

    std::sort(generators.begin(), generators.end(), HaplotypeComp);
    std::sort(filtered.begin(), filtered.end(), HaplotypeComp);

    if (mergeOutliers_) {
        // Given the set of haplotypes clustered by identity, try collapsing
        // filtered into generators.
        for (auto& hw : filtered) {
            std::vector<double> probabilities;
            if (verbose_) std::cerr << *hw << std::endl;
            double genCov = 0;
            for (auto& hn : generators) {
                genCov += hn->Size();
                if (verbose_) std::cerr << *hn << " ";
                double p = 1;
                for (size_t a = 0; a < hw->Codons.size(); ++a) {
                    double p2 = transitions_.Transition(hn->Codons.at(a), hw->Codons.at(a));
                    if (verbose_) std::cerr << std::setw(15) << p2;
                    if (p2 > 0) p *= p2;
                }
                if (verbose_) std::cerr << " = " << std::setw(15) << p << std::endl;
                probabilities.push_back(p);
            }

            double sum = std::accumulate(probabilities.cbegin(), probabilities.cend(), 0.0);
            std::vector<double> weight;
            for (size_t i = 0; i < generators.size(); ++i)
                weight.emplace_back(1.0 * generators[i]->Size() / genCov);

            std::vector<double> probabilityWeight;
            for (size_t i = 0; i < generators.size(); ++i)
                probabilityWeight.emplace_back(weight[i] * probabilities[i] / sum);

            double sumPW =
                std::accumulate(probabilityWeight.cbegin(), probabilityWeight.cend(), 0.0);

            for (size_t i = 0; i < generators.size(); ++i) {
                const auto softp = 1.0 * hw->Size() * probabilityWeight[i] / sumPW;
                if (verbose_) std::cerr << softp << "\t";
                generators[i]->SoftCollapses += softp;
            }

            if (verbose_) std::cerr << std::endl << std::endl;
        }
    }

    if (verbose_) std::cerr << "#Haplotypes: " << generators.size() << std::endl;
    double counts = 0;
    for (auto& hn : generators)
        counts += hn->Size();
    if (verbose_) std::cerr << "#Counts: " << counts << std::endl;

    // Sort generators descending
    std::stable_sort(generators.begin(), generators.end(),
                     [](const std::shared_ptr<Haplotype>& a, const std::shared_ptr<Haplotype>& b) {
                         return a->Size() >= b->Size();
                     });

    static constexpr int alphabetSize = 26;
    bool doubleName = generators.size() > alphabetSize;
    for (size_t genNumber = 0; genNumber < generators.size(); ++genNumber) {
        auto& hn = generators.at(genNumber);
        hn->GlobalFrequency = hn->Size() / counts;
        if (doubleName) {
            hn->Name = std::string(1, 'A' + genNumber / alphabetSize) +
                       std::string(1, 'a' + genNumber % alphabetSize);
        } else {
            hn->Name = std::string(1, 'A' + genNumber);
        }
        if (verbose_) std::cerr << hn->GlobalFrequency << "\t" << hn->Size() << "\t";
        size_t numCodons = hn->Codons.size();
        for (size_t i = 0; i < numCodons; ++i) {
            for (auto& kv : variantPositions.at(i).second->aminoAcidToCodons) {
                for (auto& vc : kv.second) {
                    bool hit = hn->Codons.at(i) == vc.codon;
                    vc.haplotypeHit.push_back(hit);
                    if (hit) {
                        std::cerr << termcolor::red;
                    }
                }
            }
            if (verbose_) std::cerr << hn->Codons.at(i) << termcolor::reset << " ";
        }
        if (verbose_) std::cerr << std::endl;

        reconstructedHaplotypes_.push_back(*hn);
    }
    std::cerr << termcolor::reset;

    const auto PrintHaplotype = [&ExtractRegionFromRow, &variantPositions,
                                 this](std::shared_ptr<Haplotype> h) {
        for (const auto& name : h->Names) {
            std::cerr << name << "\t";
            const auto& row = msaByRow_.NameToRow[name];

            for (const auto& pos_var : variantPositions) {
                std::string codon = ExtractRegionFromRow(row, pos_var, 0, 3);
                std::cerr << codon;
                std::cerr << "\t";
            }
            std::cerr << std::endl;
        }
        std::cerr << std::endl;
    };

    if (verbose_) std::cerr << std::endl << "HAPLOTYPES" << std::endl;
    for (auto& hn : generators) {
        genCounts_ += hn->Names.size();
        if (verbose_) std::cerr << "HAPLOTYPE: " << hn->Name << std::endl;
        if (verbose_) PrintHaplotype(hn);
    }

    std::map<int, int> filteredCounts;

    if (verbose_) std::cerr << "FILTERED" << std::endl;
    for (auto& h : filtered) {
        filteredCounts[h->Flags] += h->Names.size();
        if (verbose_) PrintHaplotype(h);
        filteredHaplotypes_.emplace_back(*h);
    }

    int sumFiltered = 0;
    for (const auto& kv : filteredCounts) {
        sumFiltered += kv.second;
        if (kv.first & static_cast<int>(HaplotypeType::WITH_GAP)) margWithGap_ += kv.second;
        if (kv.first & static_cast<int>(HaplotypeType::WITH_HETERODUPLEX))
            margWithHetero_ += kv.second;
        if (kv.first & static_cast<int>(HaplotypeType::PARTIAL)) margPartial_ += kv.second;
        if (kv.first == static_cast<int>(HaplotypeType::LOW_COV)) lowCov_ += kv.second;
        if (kv.first & static_cast<int>(HaplotypeType::OFFTARGET)) margOfftarget_ += kv.second;
    }

    if (verbose_) {
        std::cerr << "HEALTHY, REPORTED\t\t: " << genCounts_ << std::endl;
        std::cerr << "HEALTHY, TOO LOW COVERAGE\t: " << lowCov_ << std::endl;
        std::cerr << "---" << std::endl;
        std::cerr << "ALL DAMAGED\t\t\t: " << margOfftarget_ << std::endl;
        std::cerr << "MARGINAL WITH GAPS\t\t: " << margWithGap_ << std::endl;
        std::cerr << "MARGINAL WITH HETERODUPLEXES\t: " << margWithHetero_ << std::endl;
        std::cerr << "MARGINAL PARTIAL READS\t\t: " << margPartial_ << std::endl;
        std::cerr << "---" << std::endl;
        std::cerr << "SUM\t\t\t: " << genCounts_ + sumFiltered << std::endl;
    }
}

double AminoAcidCaller::Probability(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return 0.0;

    double p = 1;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == '-' || b[i] == '-')
            p *= error_.deletion;
        else if (a[i] != b[i])
            p *= error_.substitution;
        else
            p *= error_.match;
    }
    return p;
};

std::pair<bool, bool> AminoAcidCaller::MeasurePerformance(
    const TargetGene& tg, const std::pair<std::string, int>& codon_counts, const int& codonPos,
    const int& i, const double& p, const int& coverage, const std::string& geneName,
    double* truePositives, double* falsePositives, double* falseNegative, double* trueNegative)
{
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
    const bool predictor = Predictor();
    if (variableSite) {
        if (predictor) {
            if (p < alpha)
                ++*truePositives;
            else
                ++*falseNegative;
        } else {
            if (p < alpha)
                ++*falsePositives;
            else
                ++*trueNegative;
        }
    } else if (predictor) {
        if (p < alpha)
            ++*truePositives;
        else
            ++*falseNegative;
    }

    return std::make_pair(variableSite, predictor);
}

void AminoAcidCaller::CallVariants()
{
    auto genes = targetConfig_.targetGenes;
    const size_t numExpectedMinors = targetConfig_.NumExpectedMinors();
    const bool hasExpectedMinors = numExpectedMinors > 0;

    const bool hasReference = !targetConfig_.referenceSequence.empty();
    // If no user config has been provided, use complete input region
    if (genes.empty()) {
        noConfOffset = msaByRow_.BeginPos;
        TargetGene tg(noConfOffset, msaByRow_.EndPos, "Unnamed ORF", {});
        genes.emplace_back(tg);
    }

    VariantGene curVariantGene;
    std::string geneName;
    int geneOffset = 0;

    const auto SetNewGene = [this, &geneName, &curVariantGene, &geneOffset](
        const int begin, const std::string& name) {
        geneName = name;
        if (!curVariantGene.relPositionToVariant.empty())
            variantGenes_.emplace_back(std::move(curVariantGene));
        curVariantGene = VariantGene();
        curVariantGene.geneName = name;
        curVariantGene.geneOffset = begin;
        geneOffset = begin;
    };

    const int numberOfTests = CountNumberOfTests(genes);

    double truePositives = 0;
    double falsePositives = 0;
    double falseNegative = 0;
    double trueNegative = 0;

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
            const int bi = i - msaByRow_.BeginPos;

            const int codonPos = 1 + (ri) / 3;
            curVariantGene.relPositionToVariant.emplace(
                codonPos, std::make_shared<VariantGene::VariantPosition>());
            auto& curVariantPosition = curVariantGene.relPositionToVariant.at(codonPos);

            std::map<std::string, int> codons;
            int coverage = 0;
            for (const auto& nucRow : msaByRow_.Rows) {
                const auto& row = nucRow->Bases;
                const auto CodonContains = [&row, &bi](const char x) {
                    return (row.at(bi + 0) == x || row.at(bi + 1) == x || row.at(bi + 2) == x);
                };

                // Read does not cover codon
                if (bi + 2 >= static_cast<int>(row.size()) || bi < 0) continue;
                if (CodonContains(' ')) continue;

                // Read has a deletion
                if (CodonContains('-')) continue;

                const auto codon = std::string() + row.at(bi) + row.at(bi + 1) + row.at(bi + 2);

                // Codon is bogus
                if (AAT::FromCodon.find(codon) == AAT::FromCodon.cend()) continue;
                ++coverage;

                codons[codon]++;
            }

            auto FindMajorityCall = [&codons]() {
                int max = -1;
                std::string majorCodon;
                for (const auto& codon_counts : codons) {
                    if (codon_counts.second > max) {
                        max = codon_counts.second;
                        majorCodon = codon_counts.first;
                    }
                }
                if (AAT::FromCodon.find(majorCodon) == AAT::FromCodon.cend()) {
                    return std::make_tuple(0, std::string(""), ' ');
                }
                char majorAminoAcid = AAT::FromCodon.at(majorCodon);
                return std::make_tuple(max, majorCodon, majorAminoAcid);
            };

            if (hasReference) {
                curVariantPosition->refCodon = targetConfig_.referenceSequence.substr(ai, 3);
                if (AAT::FromCodon.find(curVariantPosition->refCodon) == AAT::FromCodon.cend()) {
                    continue;
                }
                curVariantPosition->refAminoAcid = AAT::FromCodon.at(curVariantPosition->refCodon);
                int majorCoverage;
                std::string altRefCodon;
                char altRefAminoAcid;
                std::tie(majorCoverage, altRefCodon, altRefAminoAcid) = FindMajorityCall();
                if (majorCoverage == 0) continue;
                if (majorCoverage * 100.0 / coverage > maximalPerc_) {
                    curVariantPosition->altRefCodon = altRefCodon;
                    curVariantPosition->altRefAminoAcid = altRefAminoAcid;
                }
            } else {
                int majorCoverage;
                std::tie(majorCoverage, curVariantPosition->refCodon,
                         curVariantPosition->refAminoAcid) = FindMajorityCall();
                if (majorCoverage == 0) continue;
            }

            for (const auto& codon_counts : codons) {
                if (curVariantPosition->refCodon == codon_counts.first) continue;
                if (!curVariantPosition->altRefCodon.empty() &&
                    curVariantPosition->altRefCodon == codon_counts.first)
                    continue;
                auto expected =
                    coverage * Probability(curVariantPosition->refCodon, codon_counts.first);
                double p =
                    (Statistics::Fisher::fisher_exact_tiss(
                         std::ceil(codon_counts.second), std::ceil(coverage - codon_counts.second),
                         std::ceil(expected), std::ceil(coverage - expected)) *
                     numberOfTests);

                if (p > 1) p = 1;

                bool variableSite;
                bool predictorSite;
                std::tie(variableSite, predictorSite) = MeasurePerformance(
                    gene, codon_counts, codonPos, ai, p, coverage, geneName, &truePositives,
                    &falsePositives, &falseNegative, &trueNegative);

                auto StoreVariant = [this, &codon_counts, &coverage, &p, &geneName, &genes,
                                     &curVariantPosition, &codonPos]() {
                    const double freq = codon_counts.second / static_cast<double>(coverage);
                    if (debug_ || freq * 100 >= minimalPerc_) {
                        const char curAA = AAT::FromCodon.at(codon_counts.first);
                        VariantGene::VariantPosition::VariantCodon curVariantCodon;
                        curVariantCodon.codon = codon_counts.first;
                        curVariantCodon.frequency = freq;
                        curVariantCodon.pValue = p;
                        curVariantCodon.knownDRM =
                            FindDRMs(geneName, genes,
                                     DMutation(curVariantPosition->refAminoAcid, codonPos, curAA));

                        curVariantPosition->aminoAcidToCodons[curAA].push_back(curVariantCodon);
                    }
                };
                if (debug_) {
                    StoreVariant();
                } else if (p < alpha) {
                    if (drmOnly_) {
                        if (!FindDRMs(geneName, genes,
                                      DMutation(curVariantPosition->refAminoAcid, codonPos,
                                                AAT::FromCodon.at(codon_counts.first)))
                                 .empty())
                            StoreVariant();
                    } else {
                        if (predictorSite)
                            StoreVariant();
                        else if (hasExpectedMinors && variableSite)
                            StoreVariant();
                        else if (!hasExpectedMinors)
                            StoreVariant();
                    }
                }
            }
            if (!curVariantPosition->aminoAcidToCodons.empty()) {
                curVariantPosition->coverage = coverage;
                for (int j = -3; j < 6; ++j) {
                    if (i + j >= msaByRow_.BeginPos && i + j < msaByRow_.EndPos) {
                        int abs = ai + j;
                        JSON::Json msaCounts;
                        msaCounts["rel_pos"] = j;
                        msaCounts["abs_pos"] = abs;
                        msaCounts["A"] = msaByColumn_[abs][0];
                        msaCounts["C"] = msaByColumn_[abs][1];
                        msaCounts["G"] = msaByColumn_[abs][2];
                        msaCounts["T"] = msaByColumn_[abs][3];
                        msaCounts["-"] = msaByColumn_[abs][4];
                        msaCounts["N"] = msaByColumn_[abs][5];
                        if (hasReference)
                            msaCounts["wt"] =
                                std::string(1, targetConfig_.referenceSequence.at(abs));
                        else
                            msaCounts["wt"] = std::string(
                                1, Data::TagToNucleotide(msaByColumn_[abs].MaxElement()));
                        curVariantPosition->msa.push_back(msaCounts);
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
        std::ofstream outValJson("validation.json");
        outValJson << "{\"true_positive_rate\":" << tpr << ",";
        outValJson << "\"false_positive_rate\":" << fpr << ",";
        outValJson << "\"num_tests\":" << numberOfTests << ",";
        outValJson << "\"num_false_positives\":" << falsePositives << ",";
        outValJson << "\"accuracy\":" << acc << "}";
    }
    if (!curVariantGene.relPositionToVariant.empty())
        variantGenes_.emplace_back(std::move(curVariantGene));
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
    auto HapsToJson = [](const std::vector<Haplotype>& haps) {
        std::vector<Json> haplotypes;
        for (const auto& h : haps) {
            haplotypes.push_back(h.ToJson());
        }
        return haplotypes;
    };
    root["haplotypes"] = HapsToJson(reconstructedHaplotypes_);
    // root["haplotypes_low_counts"] = HapsToJson(lowCountHaplotypes_);
    // root["haplotypes_filtered"] = HapsToJson(filteredHaplotypes_);
    Json counts;
    counts["healthy_reported"] = genCounts_;
    counts["healthy_low_coverage"] = lowCov_;
    counts["all_damaged"] = margOfftarget_;
    counts["marginal_with_gaps"] = margWithGap_;
    counts["marginal_with_heteroduplexes"] = margWithHetero_;
    counts["marginal_partial_reads"] = margPartial_;
    root["haplotype_read_counts"] = counts;
    return root;
}
}
}  // ::PacBio::Juliet
