#include <Dictionaries/RangeHashedDictionaryGetItemsImpl.txx>

namespace DB
{
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int8);
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int16);
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int32);
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int64);
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int128);
INSTANTIATE_GET_ITEMS_IMPL_FOR_ATTRIBUTE_TYPE(Int256);
}
