#pragma once
#include <algorithm>
#include <numeric>
#include <vector>
namespace SourcePort {
// World batches contain material/light state and owned vectors. Sort compact
// indices, then apply each permutation cycle once; preserve stable ties and
// never repeatedly move the complete payload through merge-sort scratch.
template<class T,class Order,class Index>
void ApplyPayloadOrder(std::vector<T>& values,std::size_t begin,Order& order,Index index){
    for(std::size_t i=0;i<order.size();++i){
        if(index(order[i])==i)continue;
        T saved=std::move(values[begin+i]);auto at=i;
        while(index(order[at])!=i){
            auto next=index(order[at]);
            values[begin+at]=std::move(values[begin+next]);index(order[at])=at;at=next;
        }
        values[begin+at]=std::move(saved);index(order[at])=at;
    }
}
template<class T,class Compare>
void StableSortPayloads(std::vector<T>& values,std::size_t begin,std::size_t end,Compare compare){
    if(end-begin<2)return;
    std::vector<std::size_t> order(end-begin);std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return compare(values[begin+a],values[begin+b]);});
    ApplyPayloadOrder(values,begin,order,[](auto& i)->auto&{return i;});
}
}
