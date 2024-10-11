/* Modification */
/*
 * custom-cc-tag.cc
 *
 *  Created on: Mar 26, 2020
 *      Author: vamsi
 */

#include "custom-cc-tag.h"
namespace ns3{

TypeId
MyCcTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MyCcTag")
    .SetParent<Tag> ()
    .AddConstructor<MyCcTag> ()
    .AddAttribute ("ccCustom",
                   "Cc value",
                   EmptyAttributeValue (),
                   MakeUintegerAccessor (&MyCcTag::GetCc),
                   MakeUintegerChecker<uint8_t> ())
  ;
  return tid;
}
TypeId
MyCcTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}
uint32_t
MyCcTag::GetSerializedSize (void) const
{
  return 1;
}
void
MyCcTag::Serialize (TagBuffer i) const
{
  i.WriteU8 (m_ccCustom);
}
void
MyCcTag::Deserialize (TagBuffer i)
{
  m_ccCustom = i.ReadU8 ();
}
void
MyCcTag::Print (std::ostream &os) const
{
  os << "v=" << (uint32_t)m_ccCustom;
}
void
MyCcTag::SetCc (uint8_t value)
{
  m_ccCustom = value;
}
uint8_t
MyCcTag::GetCc (void) const
{
  return m_ccCustom;
}
/* Modification */

}