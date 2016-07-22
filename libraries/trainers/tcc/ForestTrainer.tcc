////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Machine Learning Library (EMLL)
//  File:     ForestTrainer.tcc (trainers)
//  Authors:  Ofer Dekel
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#define VERY_VERBOSE

namespace trainers
{    
    template <typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::ForestTrainer(const BoosterType& booster, const ForestTrainerParameters& parameters) :
        _booster(booster), _parameters(parameters), _forest(std::make_shared<predictors::SimpleForestPredictor>())
    {}

    template <typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Update(dataset::GenericRowDataset::Iterator exampleIterator)
    {
        // convert data from iterator to dense dataset with metadata (weak weight / weak label) associated with each example
        LoadData(exampleIterator);

        // boosting loop (outer loop)
        for(size_t round = 0; round < _parameters.numRounds; ++round)
        {
            // call the booster and compute sums for the entire dataset
            Sums sums = SetWeakWeightsLabels();

            // use the computed sums to calaculate the bias term, set it in the forest and the dataset
            double bias = sums.sumWeightedLabels / sums.sumWeights;
            _forest->AddToBias(bias);
            UpdateCurrentOutputs(bias);

#ifdef VERY_VERBOSE
            _dataset.Print(std::cout);
            std::cout << "\nBoosting iteration\n";
            _forest->PrintLine(std::cout, 1);
#endif

            // find split candidate for root node and push it onto the priority queue
            auto rootSplit = GetBestSplitCandidateAtNode(_forest->GetNewRootId(), Range{0, _dataset.NumExamples()}, sums);

            // check for positive gain 
            if(rootSplit.gain < _parameters.minSplitGain || _parameters.maxSplitsPerRound == 0)
            {
                return;
            }

            // reset the queue and add the root split from the graph
            if(_queue.size() > 0)
            {
                _queue = PriorityQueue();
            }
            _queue.push(std::move(rootSplit));

            // start performing splits until the maximum is reached or the queue is empty
            PerformSplits(_parameters.maxSplitsPerRound);
        }
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeRanges::NodeRanges(const Range& totalRange) : _total(totalRange)
    {}

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    typename ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Range ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeRanges::GetChildRange(size_t childPosition) const
    {
        if (childPosition == 0)
        {
            return Range{ _total.firstIndex, _size0 };
        }
        else
        {
            return Range{ _total.firstIndex+_size0, _total.size-_size0 };
        }
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeRanges::SetSize0(size_t value)
    {
        _size0 = value;
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums::Increment(const dataset::WeightLabel& weightLabel)
    {
        sumWeights += weightLabel.weight;
        sumWeightedLabels += weightLabel.weight * weightLabel.label;
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    typename ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums::operator-(const Sums& other) const
    {
        Sums difference;
        difference.sumWeights = sumWeights - other.sumWeights;
        difference.sumWeightedLabels = sumWeightedLabels - other.sumWeightedLabels;
        return difference;
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeStats::SetChildSums(std::vector<Sums> childSums) 
    { 
        _childSums = childSums; 
    } 

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeStats::NodeStats(const Sums& totalSums) : _totalSums(totalSums), _childSums(2)
    {}

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    typename const ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums& ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeStats::GetChildSums(size_t position) const
    {
        return _childSums[position];
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::SplitCandidate::SplitCandidate(SplittableNodeId nodeId, Range totalRange, Sums totalSums) : gain(0), nodeId(nodeId), ranges(totalRange), stats(totalSums)
    {}

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::LoadData(dataset::GenericRowDataset::Iterator exampleIterator)
    {
        while (exampleIterator.IsValid())
        {
            const auto& example = exampleIterator.Get();

            // TODO this code breaks encapsulation - give ForestTrainer a ctor that takes an IDataVector
            auto denseDataVector = std::make_unique<dataset::DoubleDataVector>(example.GetDataVector().ToArray());

            ExampleMetaData metaData;
            metaData.strong = example.GetMetaData();
            metaData.currentOutput = _forest->Compute(*denseDataVector);

            _dataset.AddExample(ForestTrainerExample(std::move(denseDataVector), metaData));

            exampleIterator.Next();
        }
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    typename ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::SetWeakWeightsLabels()
    {
        Sums sums;

        for (uint64_t rowIndex = 0; rowIndex < _dataset.NumExamples(); ++rowIndex)
        {
            auto& metaData = _dataset[rowIndex].GetMetaData();
            metaData.weak = _booster.GetWeakWeightLabel(metaData.strong, metaData.currentOutput);           
            sums.Increment(metaData.weak);
        }

        if(sums.sumWeights == 0.0)
        {
            throw utilities::InputException(utilities::InputExceptionErrors::badData, "sum of weights in data is zero");
        }

        return sums;
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::UpdateCurrentOutputs(double value)
    {
        for (uint64_t rowIndex = 0; rowIndex < _dataset.NumExamples(); ++rowIndex)
        {
            auto& example = _dataset[rowIndex];
            example.GetMetaData().currentOutput += value;
        }
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::UpdateCurrentOutputs(Range range, const EdgePredictorType& edgePredictor)
    {
        for (uint64_t rowIndex = range.firstIndex; rowIndex < range.firstIndex + range.size; ++rowIndex)
        {
            auto& example = _dataset[rowIndex];
            example.GetMetaData().currentOutput += edgePredictor.Compute(example.GetDataVector());
        }
    }

    template <typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::PerformSplits(size_t maxSplits)
    {
        // count splits
        size_t splitCount = 0;

        // splitting loop (inner loop)
        while (!_queue.empty())
        {

#ifdef VERY_VERBOSE
            std::cout << "\nSplit iteration\n";
            _queue.PrintLine(std::cout, 1);
#endif

            auto splitCandidate = _queue.top();
            _queue.pop();

            const auto& stats = splitCandidate.stats;
            const auto& ranges = splitCandidate.ranges;

            // sort the data according to the performed split and update the metadata to reflect this change
            SortNodeDataset(ranges.GetTotalRange(), splitCandidate.splitRule);

            // update current output field in metadata
            auto edgePredictors = GetEdgePredictors(stats);
            for (size_t i = 0; i<2; ++i)
            {
                UpdateCurrentOutputs(ranges.GetChildRange(i), edgePredictors[i]);
            }

            // have the forest perform the split
            using SplitAction = predictors::SimpleForestPredictor::SplitAction;
            SplitAction splitAction(splitCandidate.nodeId, splitCandidate.splitRule, edgePredictors);
            auto interiorNodeIndex = _forest->Split(splitAction);

#ifdef VERY_VERBOSE
            _dataset.Print(std::cout, 1);
            std::cout << "\n";
            _forest->PrintLine(std::cout, 1);
#endif

            // if max number of splits reached, exit the loop
            if (++splitCount >= maxSplits)
            {
                break;
            }

            // queue new split candidates
            for (size_t i = 0; i<2; ++i)
            {
                auto splitCandidate = GetBestSplitCandidateAtNode(_forest->GetChildId(interiorNodeIndex, i), ranges.GetChildRange(i), stats.GetChildSums(i));
                if (splitCandidate.gain > _parameters.minSplitGain)
                {
                    _queue.push(std::move(splitCandidate));
                }
            }
        }
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::SortNodeDataset(Range range, const SplitRuleType& splitRule)
    {
        if(splitRule.NumOutputs() == 2)
        {
            _dataset.Partition([splitRule](const ForestTrainerExample& example) { return splitRule.Compute(example.GetDataVector()) == 0 ? true : false; },
                               range.firstIndex,
                               range.size);
        }
        else
        {
            _dataset.Sort([splitRule](const ForestTrainerExample& example) { return splitRule.Compute(example.GetDataVector()); },
                          range.firstIndex,
                          range.size);
        }
    }

    //
    // debugging code
    //
 
    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::ExampleMetaData::Print(std::ostream & os) const
    {
        os << "(" << strong.weight << ", " << strong.label << ", " << weak.weight << ", " << weak.label << ", " << currentOutput << ")";
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::Sums::Print(std::ostream& os) const
    {
        os << "sumWeights = " << sumWeights << ", sumWeightedLabels = " << sumWeightedLabels;
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::NodeStats::PrintLine(std::ostream& os, size_t tabs) const
    {
        os << std::string(tabs * 4, ' ') << "stats:\n";

        os << std::string((tabs+1) * 4, ' ') <<  "sums:\t";
        _totalSums.Print(os);
        os << "\n";

        os << std::string((tabs+1) * 4, ' ') <<  "sums0:\t";
        _childSums[0].Print(os);
        os << "\n";

        os << std::string((tabs+1) * 4, ' ') <<  "sums1:\t";
        _childSums[1].Print(os);
        os << "\n";
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::SplitCandidate::PrintLine(std::ostream& os, size_t tabs) const
    {
        os << std::string(tabs * 4, ' ') << "gain = " << gain << "\n";
        os << std::string(tabs * 4, ' ') << "node = ";
        nodeId.Print(os);
        os << "\n";
        splitRule.PrintLine(os, tabs);
        stats.PrintLine(os, tabs);
    }

    template<typename SplitRuleType, typename EdgePredictorType, typename BoosterType>
    void ForestTrainer<SplitRuleType, EdgePredictorType, BoosterType>::PriorityQueue::PrintLine(std::ostream& os, size_t tabs) const
    {
        os << std::string(tabs * 4, ' ') << "Priority Queue Size: " << size() << "\n";

        for(const auto& candidate : std::priority_queue<SplitCandidate>::c) // c is a protected member of std::priority_queue
        {
            os << "\n";
            candidate.PrintLine(os, tabs + 1);
            os << "\n";
        }
    }
}
