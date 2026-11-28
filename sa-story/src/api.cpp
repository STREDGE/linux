// Интерфейс AML «SAStorySwat» для SA Living (отряд с ИИ): режим, места базы, сотрудники, розыск NPC, рация, тревога.
#include <mod/amlmod.h>
#include <mod/interface.h>

#include <swat/swat_api.h>

#include "base.h"
#include "layout.h"
#include "log.h"
#include "story.h"
#include "wanted.h"

namespace api
{
namespace
{
    bool ModeOn() { return story::On(); }
    bool BaseLive() { return story::On() && base::Live(); }
    bool GetPlace(int id, SwatPlace* out)
    {
        if(!out) return false;
        float u = 0, v = 0, dz = 0, h = 0;
        switch(id)
        {
            case SWAT_PLACE_CENTER: break;
            case SWAT_PLACE_MEDBAY: u = layout::kMedU; v = layout::kMedV - 4; break;
            case SWAT_PLACE_ARMORY: u = layout::kArmoryU; v = layout::kArmoryV - 3.2f; break;
            case SWAT_PLACE_CELL: u = layout::kCellDropU; v = layout::kCellDropV; break;
            case SWAT_PLACE_HELIPAD: u = layout::kPadU; v = layout::kPadV; dz = layout::kRoofZ - layout::kZ; break;
            case SWAT_PLACE_GATE: u = layout::kGateU; v = layout::kGateV - 6; break;
            case SWAT_PLACE_GARAGE: u = -52; v = 10; h = 90; break;
            case SWAT_PLACE_HQ_DOOR: u = layout::kFrankU; v = layout::kFrankV; break;
            case SWAT_PLACE_PARKING: u = layout::kParkU; v = layout::kParkV; break;
            case SWAT_PLACE_REST: u = layout::kRestU; v = layout::kRestV - 2; break;
            default: return false;
        }
        game::Vec w = layout::W(u, v, dz);
        out->x = w.x; out->y = w.y; out->z = w.z; out->heading = layout::H(h);
        return true;
    }
    int  StaffCount() { return base::StaffCount(); }
    int  StaffRef(int i) { return base::StaffRef(i); }
    const char* StaffName(int i) { return base::StaffName(i); }
    int  StaffKind(int i) { return base::StaffKind(i); }
    int  StaffModel(int i) { return base::StaffModel(i); }
    int  StaffIndexOf(int ref) { return base::StaffIndexOf(ref); }
    void ReleaseStaff(int ref) { base::ReleaseStaff(ref); }
    void ReturnStaff(int ref) { base::ReturnStaff(ref); }
    int  NpcStars(int ref) { return wanted::Stars(ref); }
    void SetNpcStars(int ref, int stars, const char* why) { wanted::SetStars(ref, stars, why); }
    void CallBackup(int kind, float x, float y, float z) { wanted::CallBackup(kind, x, y, z, "отряд по рации"); }
    void Arrested(int ref) { wanted::Arrested(ref); }
    void Delivered(int ref) { wanted::Delivered(ref); }
    bool AlarmOn() { return base::AlarmOn(); }
    void RaiseAlarm(const char* why) { base::RaiseAlarm(why); }

    SAStorySwatApi g_api = { 1, ModeOn, BaseLive, GetPlace, StaffCount, StaffRef, StaffName, StaffKind, StaffModel, StaffIndexOf,
                             ReleaseStaff, ReturnStaff, NpcStars, SetNpcStars, CallBackup, Arrested, Delivered, AlarmOn, RaiseAlarm };
}

void Register()
{
    RegisterInterface(SASTORY_SWAT_INTERFACE, &g_api);
    LOG("СЕССИЯ", "интерфейс «%s» для SA Living зарегистрирован", SASTORY_SWAT_INTERFACE);
}
}
