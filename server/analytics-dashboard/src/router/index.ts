import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import LoginView from '@/views/LoginView.vue'
import OverviewView from '@/views/OverviewView.vue'
import AdoptionView from '@/views/AdoptionView.vue'
import PrintsView from '@/views/PrintsView.vue'
import CrashesView from '@/views/CrashesView.vue'
import ReleasesView from '@/views/ReleasesView.vue'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/login', name: 'login', component: LoginView },
    {
      path: '/',
      name: 'overview',
      component: OverviewView,
      meta: { requiresAuth: true }
    },
    {
      path: '/adoption',
      name: 'adoption',
      component: AdoptionView,
      meta: { requiresAuth: true }
    },
    {
      path: '/prints',
      name: 'prints',
      component: PrintsView,
      meta: { requiresAuth: true }
    },
    {
      path: '/crashes',
      name: 'crashes',
      component: CrashesView,
      meta: { requiresAuth: true }
    },
    {
      path: '/releases',
      name: 'releases',
      component: ReleasesView,
      meta: { requiresAuth: true }
    },
    {
      path: '/memory',
      name: 'memory',
      component: () => import('@/views/MemoryView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/hardware',
      name: 'hardware',
      component: () => import('@/views/HardwareView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/engagement',
      name: 'engagement',
      component: () => import('@/views/EngagementView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/reliability',
      name: 'reliability',
      component: () => import('@/views/ReliabilityView.vue'),
      meta: { requiresAuth: true }
    }
  ]
})

router.beforeEach((to) => {
  const auth = useAuthStore()
  if (to.meta.requiresAuth && !auth.isAuthenticated) {
    return { name: 'login' }
  }
})
